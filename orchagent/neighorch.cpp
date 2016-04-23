#include "neighorch.h"

#include "logger.h"

extern sai_neighbor_api_t*         sai_neighbor_api;
extern sai_next_hop_api_t*         sai_next_hop_api;

void NeighOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    if (consumer.m_toSync.empty())
        return;

    if (!m_portsOrch->isInitDone())
        return;

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

        if (!m_portsOrch->getPort(alias, p))
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

        IpAddress ip_address(key.substr(found+1));
        if (!ip_address.isV4())
        {
            it = consumer.m_toSync.erase(it);
            continue;
        }

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
            /* Cannot locate the neighbor */
            else
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

    Port p;
    m_portsOrch->getPort(alias, p);

    sai_neighbor_entry_t neighbor_entry;
    neighbor_entry.rif_id = p.m_rif_id;
    neighbor_entry.ip_address.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    neighbor_entry.ip_address.addr.ip4 = ip_address.getV4Addr();

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

        SWSS_LOG_NOTICE("Create neighbor entry rid:%llx alias:%s ip:%s\n", p.m_rif_id, alias.c_str(), ip_address.to_string().c_str());

        sai_attribute_t next_hop_attrs[3];
        next_hop_attrs[0].id = SAI_NEXT_HOP_ATTR_TYPE;
        next_hop_attrs[0].value.s32 = SAI_NEXT_HOP_IP;
        next_hop_attrs[1].id = SAI_NEXT_HOP_ATTR_IP;
        next_hop_attrs[1].value.ipaddr.addr_family= SAI_IP_ADDR_FAMILY_IPV4;
        next_hop_attrs[1].value.ipaddr.addr.ip4 = ip_address.getV4Addr();
        next_hop_attrs[2].id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
        next_hop_attrs[2].value.oid = p.m_rif_id;

        sai_object_id_t next_hop_id;
        status = sai_next_hop_api->create_next_hop(&next_hop_id, 3, next_hop_attrs);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create next hop ip:%s rid:%llx\n", ip_address.to_string().c_str(), p.m_rif_id);

            status = sai_neighbor_api->remove_neighbor_entry(&neighbor_entry);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove neighbor entry rid:%llx alias:%s ip:%s\n", p.m_rif_id, alias.c_str(), ip_address.to_string().c_str());
            }
            return false;
        }

        SWSS_LOG_NOTICE("Create next hop ip:%s rid:%llx\n", ip_address.to_string().c_str(), p.m_rif_id);
        m_routeOrch->createNextHopEntry(ip_address, next_hop_id);

        m_syncdNeighbors[neighborEntry] = macAddress;
    }
    else
    {
        // TODO: The neighbor entry is already there
        // TODO: MAC change
    }

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

    if (m_routeOrch->getNextHopRefCount(ip_address))
    {
        SWSS_LOG_ERROR("Neighbor is still referenced ip:%s\n", ip_address.to_string().c_str());
        return false;
    }

    Port p;
    if (!m_portsOrch->getPort(alias, p))
    {
        SWSS_LOG_ERROR("Failed to locate port alias:%s\n", alias.c_str());
        return false;
    }

    sai_neighbor_entry_t neighbor_entry;
    neighbor_entry.rif_id = p.m_rif_id;
    neighbor_entry.ip_address.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    neighbor_entry.ip_address.addr.ip4 = ip_address.getV4Addr();

    sai_object_id_t next_hop_id = m_routeOrch->getNextHopEntry(ip_address).next_hop_id;
    status = sai_next_hop_api->remove_next_hop(next_hop_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        /* When next hop is not found, we continue to remove neighbor entry. */
        if (status == SAI_STATUS_ITEM_NOT_FOUND)
        {
            SWSS_LOG_ERROR("Failed to locate next hop nhid:%llx\n", next_hop_id);
        }
        else
        {
            SWSS_LOG_ERROR("Failed to remove next hop nhid:%llx\n", next_hop_id);
            return false;
        }
    }

    status = sai_neighbor_api->remove_neighbor_entry(&neighbor_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        if (status == SAI_STATUS_ITEM_NOT_FOUND)
        {
            SWSS_LOG_ERROR("Failed to locate neigbor entry rid:%llx ip:%s\n", p.m_rif_id, ip_address.to_string().c_str());
            return true;
        }

        SWSS_LOG_ERROR("Failed to remove neighbor entry rid:%llx ip:%s\n", p.m_rif_id, ip_address.to_string().c_str());

        sai_attribute_t attr;
        attr.id = SAI_NEIGHBOR_ATTR_DST_MAC_ADDRESS;
        memcpy(attr.value.mac, m_syncdNeighbors[neighborEntry].getMac(), 6);

        status = sai_neighbor_api->create_neighbor_entry(&neighbor_entry, 1, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create neighbor entry mac:%s\n", m_syncdNeighbors[neighborEntry].to_string().c_str());
        }
        return false;
    }

    m_syncdNeighbors.erase(neighborEntry);
    m_routeOrch->removeNextHopEntry(ip_address);

    return true;
}
