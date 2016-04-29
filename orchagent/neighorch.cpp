#include "neighorch.h"

#include "logger.h"

#include "assert.h"

extern sai_neighbor_api_t*         sai_neighbor_api;
extern sai_next_hop_api_t*         sai_next_hop_api;
extern sai_next_hop_group_api_t*   sai_next_hop_group_api;

bool NeighOrch::contains(IpAddress ipAddress)
{
    IpAddresses ip_addresses(ipAddress.to_string());
    return contains(ip_addresses);
}

bool NeighOrch::contains(IpAddresses ipAddresses)
{
    if (m_syncdNextHops.find(ipAddresses) == m_syncdNextHops.end())
        return false;

    return true;
}

bool NeighOrch::addNextHop(IpAddress ipAddress, Port port)
{
    SWSS_LOG_ENTER();

    assert(!contains(ipAddress));

    sai_attribute_t next_hop_attrs[3];
    next_hop_attrs[0].id = SAI_NEXT_HOP_ATTR_TYPE;
    next_hop_attrs[0].value.s32 = SAI_NEXT_HOP_IP;
    next_hop_attrs[1].id = SAI_NEXT_HOP_ATTR_IP;
    next_hop_attrs[1].value.ipaddr.addr_family= SAI_IP_ADDR_FAMILY_IPV4;
    next_hop_attrs[1].value.ipaddr.addr.ip4 = ipAddress.getV4Addr();
    next_hop_attrs[2].id = SAI_NEXT_HOP_ATTR_ROUTER_INTERFACE_ID;
    next_hop_attrs[2].value.oid = port.m_rif_id;

    sai_object_id_t next_hop_id;
    sai_status_t status = sai_next_hop_api->create_next_hop(&next_hop_id, 3, next_hop_attrs);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create next hop entry ip:%s rid%llx\n",
                       ipAddress.to_string().c_str(), port.m_rif_id);
        return false;
    }

    NextHopEntry next_hop_entry;
    next_hop_entry.next_hop_id = next_hop_id;
    next_hop_entry.ref_count = 0;
    IpAddresses ip_addresses(ipAddress.to_string());
    m_syncdNextHops[ip_addresses] = next_hop_entry;

    return true;
}

bool NeighOrch::addNextHopGroup(IpAddresses ipAddresses)
{
    SWSS_LOG_ENTER();

    assert(!contains(ipAddresses));

    if (m_nextHopGroupCount > NHGRP_MAX_SIZE)
    {
        return false;
    }

    vector<sai_object_id_t> next_hop_ids;
    set<IpAddress> next_hop_set = ipAddresses.getIpAddresses();

    /* Assert each IP address exists in m_syncdNextHops table,
     * and add the corresponding next_hop_id to next_hop_ids. */
    for (auto it = next_hop_set.begin(); it != next_hop_set.end(); it++)
    {
        IpAddresses tmp_ip((*it).to_string());

        if (!contains(tmp_ip))
        {
            SWSS_LOG_NOTICE("Failed to get next hop entry ip:%s",
                    tmp_ip.to_string().c_str());
            return false;
        }

        sai_object_id_t next_hop_id = m_syncdNextHops[tmp_ip].next_hop_id;
        next_hop_ids.push_back(next_hop_id);
    }

    sai_attribute_t nhg_attr;
    vector<sai_attribute_t> nhg_attrs;

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_ECMP;
    nhg_attrs.push_back(nhg_attr);

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_NEXT_HOP_LIST;
    nhg_attr.value.objlist.count = next_hop_ids.size();
    nhg_attr.value.objlist.list = next_hop_ids.data();
    nhg_attrs.push_back(nhg_attr);

    sai_object_id_t next_hop_group_id;
    sai_status_t status = sai_next_hop_group_api->
            create_next_hop_group(&next_hop_group_id, nhg_attrs.size(), nhg_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create next hop group nh:%s\n",
                       ipAddresses.to_string().c_str());
        return false;
    }

    m_nextHopGroupCount ++;
    SWSS_LOG_NOTICE("Create next hop group nhgid:%llx nh:%s \n",
                    next_hop_group_id, ipAddresses.to_string().c_str());

    /* Increate the ref_count for the next hops used by the next hop group. */
    for (auto it = next_hop_set.begin(); it != next_hop_set.end(); it++)
    {
        IpAddresses tmp_ip((*it).to_string());
        m_syncdNextHops[tmp_ip].ref_count ++;
    }

    /*
     * Initialize the next hop gruop structure with ref_count as 0. This
     * count will increase once the route is successfully syncd.
     */
    NextHopEntry next_hop_entry;
    next_hop_entry.next_hop_id = next_hop_group_id;
    next_hop_entry.ref_count = 0;
    m_syncdNextHops[ipAddresses] = next_hop_entry;

    return true;
}

bool NeighOrch::removeNextHop(IpAddress ipAddress)
{
    SWSS_LOG_ENTER();

    assert(contains(ipAddress));

    IpAddresses ip_addresses(ipAddress.to_string());

    if (m_syncdNextHops[ip_addresses].ref_count > 0)
    {
        SWSS_LOG_ERROR("Failed to remove still referenced next hop entry ip:%s",
                       ip_addresses.to_string().c_str());
        return false;
    }

    m_syncdNextHops.erase(ip_addresses);
    return true;
}

bool NeighOrch::removeNextHopGroup(IpAddresses ipAddresses)
{
    SWSS_LOG_ENTER();

    assert(contains(ipAddresses));

    if (m_syncdNextHops[ipAddresses].ref_count == 0)
    {
        sai_object_id_t next_hop_group_id = m_syncdNextHops[ipAddresses].next_hop_id;
        sai_status_t status = sai_next_hop_group_api->remove_next_hop_group(next_hop_group_id);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove next hop group nhgid:%llx\n", next_hop_group_id);
            return false;
        }

        m_nextHopGroupCount --;

        set<IpAddress> ip_address_set = ipAddresses.getIpAddresses();
        for (auto it = ip_address_set.begin(); it != ip_address_set.end(); it++)
        {
            IpAddresses ip_address((*it).to_string());
            (m_syncdNextHops[ip_address]).ref_count --;
        }

        m_syncdNextHops.erase(ipAddresses);
    }

    return true;
}

sai_object_id_t NeighOrch::getNextHopId(IpAddresses ipAddresses)
{
    assert(contains(ipAddresses));
    return m_syncdNextHops[ipAddresses].next_hop_id;
}

void NeighOrch::increaseNextHopRefCount(IpAddresses ipAddresses)
{
    assert(contains(ipAddresses));
    m_syncdNextHops[ipAddresses].ref_count ++;
}

void NeighOrch::decreaseNextHopRefCount(IpAddresses ipAddresses)
{
    assert(contains(ipAddresses));
    m_syncdNextHops[ipAddresses].ref_count --;
}

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

        if (!addNextHop(ip_address, p))
        {
            status = sai_neighbor_api->remove_neighbor_entry(&neighbor_entry);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove neighbor entry rid:%llx alias:%s ip:%s\n", p.m_rif_id, alias.c_str(), ip_address.to_string().c_str());
            }
            return false;
        }

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
    IpAddresses ip_addresses(ip_address.to_string());
    string alias = neighborEntry.alias;

    if (m_syncdNeighbors.find(neighborEntry) == m_syncdNeighbors.end())
        return true;

    if (m_syncdNextHops[ip_addresses].ref_count > 0)
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

    sai_object_id_t next_hop_id = m_syncdNextHops[ip_addresses].next_hop_id;
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
    removeNextHop(ip_address);

    return true;
}
