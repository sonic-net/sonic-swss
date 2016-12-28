#include <assert.h>
#include "neighorch.h"
#include "logger.h"
#include "swssnet.h"

extern sai_neighbor_api_t*         sai_neighbor_api;
extern sai_next_hop_api_t*         sai_next_hop_api;

extern PortsOrch *gPortsOrch;

NeighOrch::NeighOrch(DBConnector *db, string tableName, IntfsOrch *intfsOrch) :
        Orch(db, tableName), m_intfsOrch(intfsOrch)
{
    SWSS_LOG_ENTER();
}

bool NeighOrch::hasNextHop(IpAddress ipAddress)
{
    return m_syncdNextHops.find(ipAddress) != m_syncdNextHops.end();
}

bool NeighOrch::addNextHop(IpAddress ipAddress, string alias)
{
    SWSS_LOG_ENTER();

    assert(!hasNextHop(ipAddress));
    sai_object_id_t rif_id = m_intfsOrch->getRouterIntfsId(alias);

    sai_attribute_t next_hop_attrs[3];
    next_hop_attrs[0].id = SAI_NEXT_HOP_ATTR_TYPE;
    next_hop_attrs[0].value.s32 = SAI_NEXT_HOP_IP;
    next_hop_attrs[1].id = SAI_NEXT_HOP_ATTR_IP;
    copy(next_hop_attrs[1].value.ipaddr, ipAddress);
    next_hop_attrs[2].id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
    next_hop_attrs[2].value.oid = rif_id;

    sai_object_id_t next_hop_id;
    sai_status_t status = sai_next_hop_api->create_next_hop(&next_hop_id, 3, next_hop_attrs);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create next hop entry ip:%s rid:%lx",
                       ipAddress.to_string().c_str(), rif_id);
        return false;
    }

    SWSS_LOG_NOTICE("Create next hop entry id:%lx, ip:%s, rid:%lx\n",
                    next_hop_id, ipAddress.to_string().c_str(), rif_id);

    NextHopEntry next_hop_entry;
    next_hop_entry.next_hop_id = next_hop_id;
    next_hop_entry.ref_count = 0;
    m_syncdNextHops[ipAddress] = next_hop_entry;

    m_intfsOrch->increaseRouterIntfsRefCount(alias);

    return true;
}

bool NeighOrch::removeNextHop(IpAddress ipAddress, string alias)
{
    SWSS_LOG_ENTER();

    assert(hasNextHop(ipAddress));

    if (m_syncdNextHops[ipAddress].ref_count > 0)
    {
        SWSS_LOG_ERROR("Failed to remove still referenced next hop entry ip:%s",
                       ipAddress.to_string().c_str());
        return false;
    }

    m_syncdNextHops.erase(ipAddress);
    m_intfsOrch->decreaseRouterIntfsRefCount(alias);
    return true;
}

sai_object_id_t NeighOrch::getNextHopId(IpAddress ipAddress)
{
    assert(hasNextHop(ipAddress));
    return m_syncdNextHops[ipAddress].next_hop_id;
}

int NeighOrch::getNextHopRefCount(IpAddress ipAddress)
{
    assert(hasNextHop(ipAddress));
    return m_syncdNextHops[ipAddress].ref_count;
}

void NeighOrch::increaseNextHopRefCount(IpAddress ipAddress)
{
    assert(hasNextHop(ipAddress));
    m_syncdNextHops[ipAddress].ref_count ++;
}

void NeighOrch::decreaseNextHopRefCount(IpAddress ipAddress)
{
    assert(hasNextHop(ipAddress));
    m_syncdNextHops[ipAddress].ref_count --;
}

bool NeighOrch::getNeighborEntry(const IpAddress& ipAddress, NeighborEntry& neighborEntry, MacAddress& macAddress)
{
    if (!hasNextHop(ipAddress))
    {
        return false;
    }

    for (const auto& entry : m_syncdNeighbors)
    {
        if (entry.first.ip_address == ipAddress)
        {
            neighborEntry = entry.first;
            macAddress = entry.second;
            return true;
        }
    }

    return false;
}

void NeighOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        size_t found = key.find(':');
        if (found == string::npos)
        {
            SWSS_LOG_ERROR("Failed to parse task key %s\n", key.c_str());
            it = consumer.m_toSync.erase(it);
            continue;
        }

        string alias = key.substr(0, found);
        Port p;

        if (!gPortsOrch->getPort(alias, p))
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (!p.m_rif_id)
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        IpAddress ip_address(key.substr(found+1));

        NeighborEntry neighbor_entry = { ip_address, alias };

        string op = kfvOp(t);

        if (op == SET_COMMAND)
        {
            MacAddress mac_address;
            for (auto i = kfvFieldsValues(t).begin();
                 i  != kfvFieldsValues(t).end(); i++)
            {
                if (fvField(*i) == "neigh")
                    mac_address = MacAddress(fvValue(*i));
            }

            if (m_syncdNeighbors.find(neighbor_entry) == m_syncdNeighbors.end() || m_syncdNeighbors[neighbor_entry] != mac_address)
            {
                if (addNeighbor(neighbor_entry, mac_address))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
                /* Duplicate entry */
                it = consumer.m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            if (m_syncdNeighbors.find(neighbor_entry) != m_syncdNeighbors.end())
            {
                if (removeNeighbor(neighbor_entry))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
                /* Cannot locate the neighbor */
                it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

bool NeighOrch::addNeighbor(NeighborEntry neighborEntry, MacAddress macAddress)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    IpAddress ip_address = neighborEntry.ip_address;
    string alias = neighborEntry.alias;

    sai_object_id_t rif_id = m_intfsOrch->getRouterIntfsId(alias);

    sai_neighbor_entry_t neighbor_entry;
    neighbor_entry.rif_id = rif_id;
    copy(neighbor_entry.ip_address, ip_address);

    sai_attribute_t neighbor_attr;
    neighbor_attr.id = SAI_NEIGHBOR_ATTR_DST_MAC_ADDRESS;
    memcpy(neighbor_attr.value.mac, macAddress.getMac(), 6);

    if (m_syncdNeighbors.find(neighborEntry) == m_syncdNeighbors.end())
    {
        status = sai_neighbor_api->create_neighbor_entry(&neighbor_entry, 1, &neighbor_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create neighbor entry alias:%s ip:%s\n", alias.c_str(), ip_address.to_string().c_str());
            return false;
        }

        SWSS_LOG_NOTICE("Create neighbor entry rid:%lx alias:%s ip:%s\n", rif_id, alias.c_str(), ip_address.to_string().c_str());
        m_intfsOrch->increaseRouterIntfsRefCount(alias);

        if (!addNextHop(ip_address, alias))
        {
            status = sai_neighbor_api->remove_neighbor_entry(&neighbor_entry);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove neighbor entry rid:%lx alias:%s ip:%s\n", rif_id, alias.c_str(), ip_address.to_string().c_str());
                return false;
            }
            m_intfsOrch->decreaseRouterIntfsRefCount(alias);
            return false;
        }
    }
    else
    {
        status = sai_neighbor_api->set_neighbor_attribute(&neighbor_entry, &neighbor_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to update neighbor entry rid:%lx alias:%s ip:%s\n", rif_id, alias.c_str(), ip_address.to_string().c_str());
            return false;
        }
        SWSS_LOG_NOTICE("Updated neighbor entry rid:%lx alias:%s ip:%s new mac: %s\n", rif_id, alias.c_str(), ip_address.to_string().c_str(), macAddress.to_string().c_str());
    }

    m_syncdNeighbors[neighborEntry] = macAddress;

    NeighborUpdate update = { neighborEntry, macAddress, true };
    notify(SUBJECT_TYPE_NEIGH_CHANGE, static_cast<void *>(&update));

    return true;
}

bool NeighOrch::removeNeighbor(NeighborEntry neighborEntry)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    IpAddress ip_address = neighborEntry.ip_address;
    string alias = neighborEntry.alias;

    if (m_syncdNeighbors.find(neighborEntry) == m_syncdNeighbors.end())
        return true;

    if (m_syncdNextHops[ip_address].ref_count > 0)
    {
        SWSS_LOG_INFO("Neighbor is still referenced ip:%s\n", ip_address.to_string().c_str());
        return false;
    }

    sai_object_id_t rif_id = m_intfsOrch->getRouterIntfsId(alias);

    sai_neighbor_entry_t neighbor_entry;
    neighbor_entry.rif_id = rif_id;
    copy(neighbor_entry.ip_address, ip_address);

    sai_object_id_t next_hop_id = m_syncdNextHops[ip_address].next_hop_id;
    status = sai_next_hop_api->remove_next_hop(next_hop_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        /* When next hop is not found, we continue to remove neighbor entry. */
        if (status == SAI_STATUS_ITEM_NOT_FOUND)
        {
            SWSS_LOG_ERROR("Failed to locate next hop nhid:%lx\n", next_hop_id);
        }
        else
        {
            SWSS_LOG_ERROR("Failed to remove next hop nhid:%lx\n", next_hop_id);
            return false;
        }
    }

    status = sai_neighbor_api->remove_neighbor_entry(&neighbor_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_NOT_FOUND)
        {
            SWSS_LOG_ERROR("Failed to locate neigbor entry rid:%lx ip:%s\n", rif_id, ip_address.to_string().c_str());
            return true;
        }

        SWSS_LOG_ERROR("Failed to remove neighbor entry rid:%lx ip:%s\n", rif_id, ip_address.to_string().c_str());
        return false;
    }

    NeighborUpdate update = { neighborEntry, MacAddress(), false };
    notify(SUBJECT_TYPE_NEIGH_CHANGE, static_cast<void *>(&update));

    m_syncdNeighbors.erase(neighborEntry);
    m_intfsOrch->decreaseRouterIntfsRefCount(alias);
    removeNextHop(ip_address, alias);

    return true;
}
