#include "routeorch.h"

#include "logger.h"

extern sai_route_api_t*                 sai_route_api;
extern sai_next_hop_group_api_t*        sai_next_hop_group_api;

extern sai_object_id_t gVirtualRouterId;

void RouteOrch::doTask()
{
    if (m_toSync.empty())
        return;

    auto it = m_toSync.begin();
    while (it != m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        /* Get notification from application */
        /* resync application:
         * When routeorch receives 'resync' message, it marks all current
         * routes as dirty and waits for 'resync complete' message. For all
         * newly received routes, if they match current dirty routes, it unmarks
         * them dirty. After receiving 'resync complete' message, it creates all
         * newly added routes and removes all dirty routes.
         */
        if (key == "resync")
        {
            if (op == "SET")
            {
                /* Mark all current routes as dirty (DEL) in m_toSync map */
                SWSS_LOG_NOTICE("Start resync routes\n");
                for (auto i = m_syncdRoutes.begin(); i != m_syncdRoutes.end(); i++)
                {
                    vector<FieldValueTuple> v;
                    auto x = KeyOpFieldsValuesTuple(i->first.to_string(), DEL_COMMAND, v);
                    m_toSync[i->first.to_string()] = x;
                }
                m_resync = true;
            }
            else
            {
                SWSS_LOG_NOTICE("Complete resync routes\n");
                m_resync = false;
            }

            it = m_toSync.erase(it);
            continue;
        }

        if (m_resync)
        {
            it++;
            continue;
        }

        IpPrefix ip_prefix = IpPrefix(key);
        if (!ip_prefix.isV4())
        {
            it = m_toSync.erase(it);
            continue;
        }

        if (op == SET_COMMAND)
        {
            IpAddresses ip_addresses;
            string alias;

            for (auto i = kfvFieldsValues(t).begin();
                 i != kfvFieldsValues(t).end(); i++)
            {
                if (fvField(*i) == "nexthop")
                    ip_addresses = IpAddresses(fvValue(*i));

                if (fvField(*i) == "ifindex")
                    alias = fvValue(*i);
            }

            // XXX: set to blackhold if nexthop is empty?
            if (ip_addresses.getSize() == 0)
            {
                it = m_toSync.erase(it);
                continue;
            }

            // XXX: cannot trust m_portsOrch->getPortIdByAlias because sometimes alias is empty
            // XXX: need to split aliases with ',' and verify the next hops?
            if (alias == "eth0" || alias == "lo" || alias == "docker0")
            {
                it = m_toSync.erase(it);
                continue;
            }

            if (m_syncdRoutes.find(ip_prefix) == m_syncdRoutes.end() || m_syncdRoutes[ip_prefix] != ip_addresses)
            {
                if (addRoute(ip_prefix, ip_addresses))
                    it = m_toSync.erase(it);
                else
                    it++;
            }
            else
                /* Duplicate entry */
                it = m_toSync.erase(it);
        }
        else if (op == DEL_COMMAND)
        {
            if (m_syncdRoutes.find(ip_prefix) != m_syncdRoutes.end())
            {
                if (removeRoute(ip_prefix))
                    it = m_toSync.erase(it);
                else
                    it++;
            }
            /* Cannot locate the route */
            else
                it = m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
            it = m_toSync.erase(it);
        }
    }
}

bool RouteOrch::createNextHopEntry(IpAddress ipAddress, sai_object_id_t nextHopId)
{
    IpAddresses ip_addresses(ipAddress.to_string());
    return createNextHopEntry(ip_addresses, nextHopId);
}

bool RouteOrch::createNextHopEntry(IpAddresses ipAddresses, sai_object_id_t nextHopGroupId)
{
    if (m_syncdNextHops.find(ipAddresses) != m_syncdNextHops.end())
    {
        SWSS_LOG_ERROR("Failed to create existed next hop entry ip:%s nhid:%llx\n", ipAddresses.to_string().c_str(), nextHopGroupId);
        return false;
    }

    NextHopEntry next_hop_entry;
    next_hop_entry.next_hop_id = nextHopGroupId;
    next_hop_entry.ref_count = 0;
    m_syncdNextHops[ipAddresses] = next_hop_entry;
    return true;
}

bool RouteOrch::removeNextHopEntry(IpAddress ipAddress)
{
    IpAddresses ip_addresses(ipAddress.to_string());

    if (m_syncdNextHops.find(ip_addresses) == m_syncdNextHops.end())
    {
        SWSS_LOG_ERROR("Failed to remove absent next hop entry ip:%s\n", ip_addresses.to_string().c_str());
        return false;
    }

    if (getNextHopRefCount(ip_addresses) != 0)
    {
        SWSS_LOG_ERROR("Failed to remove referenced next hop entry ip:%s", ip_addresses.to_string().c_str());
        return false;
    }

    m_syncdNextHops.erase(ip_addresses);
    return true;
}

bool RouteOrch::removeNextHopEntry(IpAddresses ipAddresses)
{
    if (m_syncdNextHops.find(ipAddresses) == m_syncdNextHops.end())
    {
        SWSS_LOG_ERROR("Failed to remove absent next hop entry ip:%s\n", ipAddresses.to_string().c_str());
        return false;
    }

    if (ipAddresses.getSize() > 1 && getNextHopRefCount(ipAddresses) == 0)
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

int RouteOrch::getNextHopRefCount(IpAddress ipAddress)
{
    IpAddresses ip_addresses(ipAddress.to_string());
    return getNextHopRefCount(ip_addresses);
}

int RouteOrch::getNextHopRefCount(IpAddresses ipAddresses)
{
    return m_syncdNextHops[ipAddresses].ref_count;
}

NextHopEntry RouteOrch::getNextHopEntry(IpAddress ipAddress)
{
    IpAddresses ip_addresses(ipAddress.to_string());
    return getNextHopEntry(ip_addresses);
}

NextHopEntry RouteOrch::getNextHopEntry(IpAddresses ipAddresses)
{
    return m_syncdNextHops[ipAddresses];
}

bool RouteOrch::addRoute(IpPrefix ipPrefix, IpAddresses nextHops)
{
    /* nhid indicates the next hop id or next hop group id of this route */
    sai_object_id_t next_hop_id;
    auto it_route = m_syncdRoutes.find(ipPrefix);
    auto it_nxhop = m_syncdNextHops.find(nextHops);

    /*
     * If next hop group cannot be found in m_syncdNextHops,
     * then we need to add it.
     */
    if (it_nxhop == m_syncdNextHops.end())
    {
        NextHopEntry next_hop_entry;
        vector<sai_object_id_t> next_hop_ids;
        set<IpAddress> next_hop_set = nextHops.getIpAddresses();

        for (auto it = next_hop_set.begin(); it != next_hop_set.end(); it++)
        {
            IpAddresses tmp_ip((*it).to_string());
            if (m_syncdNextHops.find(tmp_ip) == m_syncdNextHops.end())
            {
                SWSS_LOG_ERROR("Failed to find next hop %s", (*it).to_string().c_str());
                return false;
            }

            next_hop_id = m_syncdNextHops[tmp_ip].next_hop_id;
            next_hop_ids.push_back(next_hop_id);
        }

        /*
         * If the current number of next hop groups exceeds the threshold,
         * then we need to pick up a random next hop from the next hop group as
         * the route's temporary next hop.
         */
        if (m_nextHopGroupCount > NHGRP_MAX_SIZE)
        {
            bool to_add = false;
            /*
             * A temporary entry is added when route is not in m_syncdRoutes,
             * or it is in m_syncdRoutes but the original next hop(s) is not a
             * subset of the next hop group to be added.
             */
            if (it_route != m_syncdRoutes.end())
            {
                auto tmp_set = m_syncdRoutes[ipPrefix].getIpAddresses();
                for (auto it = tmp_set.begin(); it != tmp_set.end(); it++)
                {
                    if (next_hop_set.find(*it) == next_hop_set.end())
                    {
                        to_add = true;
                    }
                }
            }
            else
            {
                to_add = true;
            }

            if (to_add)
            {
                /* Randomly pick an address from the address set. */
                auto it = next_hop_set.begin();
                advance(it, rand() % next_hop_set.size());

                /*
                 * Set the route's temporary next hop to be the randomly picked one.
                 */
                IpAddresses tmp_nxhop((*it).to_string());
                addRoute(ipPrefix, tmp_nxhop);
            }

            /*
             * Return false since the original route is not successfully added.
             */
            return false;
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

        sai_status_t status = sai_next_hop_group_api->
                create_next_hop_group(&next_hop_id, nhg_attrs.size(), nhg_attrs.data());
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to create nex thop group nh:%s\n", nextHops.to_string().c_str());
                    return false;
                }

        m_nextHopGroupCount ++;
        SWSS_LOG_NOTICE("Create next hop group nhgid:%llx nh:%s \n", next_hop_id, nextHops.to_string().c_str());

        /*
         * Increate the ref_count for the next hops used by the next hop group.
         */
        for (auto it = next_hop_set.begin(); it != next_hop_set.end(); it++)
        {
            IpAddresses tmp_ip((*it).to_string().c_str());
            m_syncdNextHops[tmp_ip].ref_count ++;
        }

        /*
         * Initialize the next hop gruop structure with ref_count as 0. This
         * count will increase once the route is successfully syncd.
         */
        next_hop_entry.ref_count = 0;
        next_hop_entry.next_hop_id = next_hop_id;
        m_syncdNextHops[nextHops] = next_hop_entry;
    }
    else
    {
        next_hop_id = m_syncdNextHops[nextHops].next_hop_id;
    }

    /* Sync the route entry */
    sai_unicast_route_entry_t route_entry;
    route_entry.vr_id = gVirtualRouterId;
    route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    route_entry.destination.addr.ip4 = ipPrefix.getIp().getV4Addr();
    route_entry.destination.mask.ip4 = ipPrefix.getMask().getV4Addr();

    sai_attribute_t route_attr;
    route_attr.id = SAI_ROUTE_ATTR_NEXT_HOP_ID;
    route_attr.value.oid = next_hop_id;

    /* If the prefix is not in m_syncdRoutes, then we need to create the route
     * for this prefix with the new next hop (group) id. If the prefix is already
     * in m_syncdRoutes, then we need to update the route with a new next hop
     * (group) id. The old next hop (group) is then not used and the reference
     * count will decrease by 1.
     */
    if (it_route == m_syncdRoutes.end())
    {
        sai_status_t status = sai_route_api->create_route(&route_entry, 1, &route_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create route %s with next hop(s) %s",
                    ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
            /* Clean up the newly created next hop (group) entry */
                        removeNextHopEntry(nextHops);
            return false;
        }

        /* Increase the ref_count for the next hop (group) entry */
        m_syncdNextHops[nextHops].ref_count ++;
        SWSS_LOG_INFO("Create route %s with next hop(s) %s",
                ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
    }
    else
    {
        sai_status_t status = sai_route_api->set_route_attribute(&route_entry, &route_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            return false;
        }
        m_syncdNextHops[nextHops].ref_count ++;

        auto it = m_syncdNextHops.find(it_route->second);
        if (it == m_syncdNextHops.end())
        {
            return false;
        }
        it->second.ref_count --;
        removeNextHopEntry(it->first);
    }
    m_syncdRoutes[ipPrefix] = nextHops;
    return true;
}

bool RouteOrch::removeRoute(IpPrefix ipPrefix)
{
    sai_unicast_route_entry_t route_entry;
    route_entry.vr_id = gVirtualRouterId;
    route_entry.destination.addr_family = SAI_IP_ADDR_FAMILY_IPV4;
    route_entry.destination.addr.ip4 = ipPrefix.getIp().getV4Addr();
    route_entry.destination.mask.ip4 = ipPrefix.getMask().getV4Addr();

    sai_status_t status = sai_route_api->remove_route(&route_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove route prefix:%s\n", ipPrefix.to_string().c_str());
        return false;
    }

    /* Remove next hop group entry if ref_count is zero */
    auto it_route = m_syncdRoutes.find(ipPrefix);
    if (it_route != m_syncdRoutes.end())
    {
        auto it_next_hop = m_syncdNextHops.find(it_route->second);
        if (it_next_hop == m_syncdNextHops.end())
        {
            SWSS_LOG_ERROR("Failed to locate next hop entry \n");
            return false;
        }
        it_next_hop->second.ref_count --;
        if (!removeNextHopEntry(it_next_hop->first))
        {
            SWSS_LOG_ERROR("Failed to remove next hop group\n");
            return false;
        }
    }
    m_syncdRoutes.erase(ipPrefix);
    return true;
}
