#include <assert.h>
#include "routeorch.h"
#include "logger.h"
#include "swssnet.h"
#include "crmorch.h"

extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gSwitchId;

extern sai_next_hop_group_api_t*    sai_next_hop_group_api;
extern sai_route_api_t*             sai_route_api;
extern sai_switch_api_t*            sai_switch_api;

extern PortsOrch *gPortsOrch;
extern CrmOrch *gCrmOrch;

/* Default maximum number of next hop groups */
#define DEFAULT_NUMBER_OF_ECMP_GROUPS   128
#define DEFAULT_MAX_ECMP_GROUP_SIZE     32

const int routeorch_pri = 5;

RouteOrch::RouteOrch(DBConnector *db, string tableName, NeighOrch *neighOrch) :
        Orch(db, tableName, routeorch_pri),
        m_neighOrch(neighOrch),
        m_nextHopGroupCount(0),
        m_resync(false)
{
    SWSS_LOG_ENTER();

    sai_attribute_t attr;
    attr.id = SAI_SWITCH_ATTR_NUMBER_OF_ECMP_GROUPS;

    sai_status_t status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to get switch attribute number of ECMP groups. \
                       Use default value. rv:%d", status);
        m_maxNextHopGroupCount = DEFAULT_NUMBER_OF_ECMP_GROUPS;
    }
    else
    {
        m_maxNextHopGroupCount = attr.value.s32;

        /*
         * ASIC specific workaround to re-calculate maximum ECMP groups
         * according to diferent ECMP mode used.
         *
         * On Mellanox platform, the maximum ECMP groups returned is the value
         * under the condition that the ECMP group size is 1. Deviding this
         * number by DEFAULT_MAX_ECMP_GROUP_SIZE gets the maximum number of
         * ECMP groups when the maximum ECMP group size is 32.
         */
        char *platform = getenv("platform");
        if (platform && strstr(platform, MLNX_PLATFORM_SUBSTRING))
        {
            m_maxNextHopGroupCount /= DEFAULT_MAX_ECMP_GROUP_SIZE;
        }
    }
    SWSS_LOG_NOTICE("Maximum number of ECMP groups supported is %d", m_maxNextHopGroupCount);

    IpPrefix default_ip_prefix("0.0.0.0/0");

    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.vr_id = gVirtualRouterId;
    unicast_route_entry.switch_id = gSwitchId;
    copy(unicast_route_entry.destination, default_ip_prefix);
    subnet(unicast_route_entry.destination, unicast_route_entry.destination);

    attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_DROP;

    status = sai_route_api->create_route_entry(&unicast_route_entry, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create IPv4 default route with packet action drop");
        throw runtime_error("Failed to create IPv4 default route with packet action drop");
    }

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);

    /* Add default IPv4 route into the m_syncdRoutes */
    m_syncdRoutes[default_ip_prefix] = IpAddresses();

    SWSS_LOG_NOTICE("Create IPv4 default route with packet action drop");

    IpPrefix v6_default_ip_prefix("::/0");

    copy(unicast_route_entry.destination, v6_default_ip_prefix);
    subnet(unicast_route_entry.destination, unicast_route_entry.destination);

    status = sai_route_api->create_route_entry(&unicast_route_entry, 1, &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create IPv6 default route with packet action drop");
        throw runtime_error("Failed to create IPv6 default route with packet action drop");
    }

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);

    /* Add default IPv6 route into the m_syncdRoutes */
    m_syncdRoutes[v6_default_ip_prefix] = IpAddresses();

    SWSS_LOG_NOTICE("Create IPv6 default route with packet action drop");
}

bool RouteOrch::hasNextHopGroup(const IpAddresses& ipAddresses) const
{
    return m_syncdNextHopGroups.find(ipAddresses) != m_syncdNextHopGroups.end();
}

sai_object_id_t RouteOrch::getNextHopGroupId(const IpAddresses& ipAddresses)
{
    assert(hasNextHopGroup(ipAddresses));
    return m_syncdNextHopGroups[ipAddresses].next_hop_group_id;
}

void RouteOrch::attach(Observer *observer, const IpAddress& dstAddr)
{
    SWSS_LOG_ENTER();

    SWSS_LOG_INFO("Attaching next hop observer for %s destination IP\n", dstAddr.to_string().c_str());

    auto observerEntry = m_nextHopObservers.find(dstAddr);

    if (observerEntry == m_nextHopObservers.end())
    {
        m_nextHopObservers.emplace(dstAddr, NextHopObserverEntry());
        observerEntry = m_nextHopObservers.find(dstAddr);

        for (auto route : m_syncdRoutes)
        {
            if (route.first.isAddressInSubnet(dstAddr))
            {
                observerEntry->second.routeTable.emplace(route.first, route.second);
            }
        }
    }

    observerEntry->second.observers.push_back(observer);

    auto route = observerEntry->second.routeTable.rbegin();
    if (route != observerEntry->second.routeTable.rend())
    {
        NextHopUpdate update = { route->first, route->second };
        observer->update(SUBJECT_TYPE_NEXTHOP_CHANGE, static_cast<void *>(&update));
    }
}

void RouteOrch::detach(Observer *observer, const IpAddress& dstAddr)
{
    SWSS_LOG_ENTER();
    auto observerEntry = m_nextHopObservers.find(dstAddr);

    if (observerEntry == m_nextHopObservers.end())
    {
        SWSS_LOG_ERROR("Failed to detach observer for %s. Entry not found.\n", dstAddr.to_string().c_str());
        assert(false);
    }

    for (auto iter = observerEntry->second.observers.begin(); iter != observerEntry->second.observers.end(); ++iter)
    {
        if (observer == *iter)
        {
            m_observers.erase(iter);
            break;
        }
    }
}

bool RouteOrch::validnexthopinNextHopGroup(const IpAddress &ipaddr)
{
    SWSS_LOG_ENTER();

    sai_object_id_t nexthop_id;
    sai_status_t status;

    for (auto nhopgroup = m_syncdNextHopGroups.begin();
         nhopgroup != m_syncdNextHopGroups.end(); ++nhopgroup)
    {

        if (!(nhopgroup->first.contains(ipaddr)))
        {
            continue;
        }

        vector<sai_attribute_t> nhgm_attrs;
        sai_attribute_t nhgm_attr;

        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
        nhgm_attr.value.oid = nhopgroup->second.next_hop_group_id;
        nhgm_attrs.push_back(nhgm_attr);

        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
        nhgm_attr.value.oid = m_neighOrch->getNextHopId(ipaddr);
        nhgm_attrs.push_back(nhgm_attr);

        status = sai_next_hop_group_api->create_next_hop_group_member(&nexthop_id, gSwitchId,
                                                                      (uint32_t)nhgm_attrs.size(),
                                                                      nhgm_attrs.data());

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to add next hop member to group %lx: %d\n",
                           nhopgroup->second.next_hop_group_id, status);
            return false;
        }

        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
        nhopgroup->second.nhopgroup_members[ipaddr] = nexthop_id;
    }

    return true;
}

bool RouteOrch::invalidnexthopinNextHopGroup(const IpAddress &ipaddr)
{
    SWSS_LOG_ENTER();

    sai_object_id_t nexthop_id;
    sai_status_t status;

    for (auto nhopgroup = m_syncdNextHopGroups.begin();
         nhopgroup != m_syncdNextHopGroups.end(); ++nhopgroup)
    {

        if (!(nhopgroup->first.contains(ipaddr)))
        {
            continue;
        }

        nexthop_id = nhopgroup->second.nhopgroup_members[ipaddr];
        status = sai_next_hop_group_api->remove_next_hop_group_member(nexthop_id);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove next hop member %lx from group %lx: %d\n",
                           nexthop_id, nhopgroup->second.next_hop_group_id, status);
            return false;
        }

        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    return true;
}

void RouteOrch::doTask(Consumer& consumer)
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
                /* Mark all current routes as dirty (DEL) in consumer.m_toSync map */
                SWSS_LOG_NOTICE("Start resync routes\n");
                for (auto i : m_syncdRoutes)
                {
                    vector<FieldValueTuple> v;
                    auto x = KeyOpFieldsValuesTuple(i.first.to_string(), DEL_COMMAND, v);
                    consumer.m_toSync[i.first.to_string()] = x;
                }
                m_resync = true;
            }
            else
            {
                SWSS_LOG_NOTICE("Complete resync routes\n");
                m_resync = false;
            }

            it = consumer.m_toSync.erase(it);
            continue;
        }

        if (m_resync)
        {
            it++;
            continue;
        }

        IpPrefix ip_prefix = IpPrefix(key);

        if (op == SET_COMMAND)
        {
            IpAddresses ip_addresses;
            string alias;

            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "nexthop")
                    ip_addresses = IpAddresses(fvValue(i));

                if (fvField(i) == "ifname")
                    alias = fvValue(i);
            }

            // TODO: set to blackhold if nexthop is empty?
            if (ip_addresses.getSize() == 0)
            {
                it = consumer.m_toSync.erase(it);
                continue;
            }

            // TODO: cannot trust m_portsOrch->getPortIdByAlias because sometimes alias is empty
            // TODO: need to split aliases with ',' and verify the next hops?
            if (alias == "eth0" || alias == "lo" || alias == "docker0")
            {
                /* If any existing routes are updated to point to the
                 * above interfaces, remove them from the ASIC. */
                if (m_syncdRoutes.find(ip_prefix) != m_syncdRoutes.end())
                {
                    if (removeRoute(ip_prefix))
                        it = consumer.m_toSync.erase(it);
                    else
                        it++;
                }
                else
                    it = consumer.m_toSync.erase(it);
                continue;
            }

            if (m_syncdRoutes.find(ip_prefix) == m_syncdRoutes.end() || m_syncdRoutes[ip_prefix] != ip_addresses)
            {
                if (addRoute(ip_prefix, ip_addresses))
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
            if (m_syncdRoutes.find(ip_prefix) != m_syncdRoutes.end())
            {
                if (removeRoute(ip_prefix))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
                /* Cannot locate the route */
                it = consumer.m_toSync.erase(it);
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
            it = consumer.m_toSync.erase(it);
        }
    }
}

void RouteOrch::notifyNextHopChangeObservers(IpPrefix prefix, IpAddresses nexthops, bool add)
{
    SWSS_LOG_ENTER();

    for (auto& entry : m_nextHopObservers)
    {
        if (!prefix.isAddressInSubnet(entry.first))
        {
            continue;
        }

        if (add)
        {
            bool update_required = false;
            NextHopUpdate update = { prefix, nexthops };

            /* Table should not be empty. Default route should always exists. */
            assert(!entry.second.routeTable.empty());

            auto route = entry.second.routeTable.find(prefix);
            if (route == entry.second.routeTable.end())
            {
                /* If added route is best match update observers */
                if (entry.second.routeTable.rbegin()->first < prefix)
                {
                    update_required = true;
                }

                entry.second.routeTable.emplace(prefix, nexthops);
            }
            else
            {
                if (route->second != nexthops)
                {
                    route->second = nexthops;
                    /* If changed route is best match update observers */
                    if (entry.second.routeTable.rbegin()->first == route->first)
                    {
                        update_required = true;
                    }
                }
            }

            if (update_required)
            {
                for (auto observer : entry.second.observers)
                {
                    observer->update(SUBJECT_TYPE_NEXTHOP_CHANGE, static_cast<void *>(&update));
                }
            }
        }
        else
        {
            auto route = entry.second.routeTable.find(prefix);
            if (route != entry.second.routeTable.end())
            {
                /* If removed route was best match find another best match route */
                if (route->first == entry.second.routeTable.rbegin()->first)
                {
                    entry.second.routeTable.erase(route);

                    /* Table should not be empty. Default route should always exists. */
                    assert(!entry.second.routeTable.empty());

                    auto route = entry.second.routeTable.rbegin();
                    NextHopUpdate update = { route->first, route->second };

                    for (auto observer : entry.second.observers)
                    {
                        observer->update(SUBJECT_TYPE_NEXTHOP_CHANGE, static_cast<void *>(&update));
                    }
                }
                else
                {
                    entry.second.routeTable.erase(route);
                }
            }
        }
    }
}

void RouteOrch::increaseNextHopRefCount(IpAddresses ipAddresses)
{
    /* Return when there is no next hop (dropped) */
    if (ipAddresses.getSize() == 0)
    {
        return;
    }
    else if (ipAddresses.getSize() == 1)
    {
        IpAddress ip_address(ipAddresses.to_string());
        m_neighOrch->increaseNextHopRefCount(ip_address);
    }
    else
    {
        m_syncdNextHopGroups[ipAddresses].ref_count ++;
    }
}
void RouteOrch::decreaseNextHopRefCount(IpAddresses ipAddresses)
{
    /* Return when there is no next hop (dropped) */
    if (ipAddresses.getSize() == 0)
    {
        return;
    }
    else if (ipAddresses.getSize() == 1)
    {
        IpAddress ip_address(ipAddresses.to_string());

        m_neighOrch->decreaseNextHopRefCount(ip_address);
    }
    else
    {
        m_syncdNextHopGroups[ipAddresses].ref_count --;
    }
}

bool RouteOrch::isRefCounterZero(const IpAddresses& ipAddresses) const
{
    if (!hasNextHopGroup(ipAddresses))
    {
        return true;
    }

    return m_syncdNextHopGroups.at(ipAddresses).ref_count == 0;
}

bool RouteOrch::addNextHopGroup(IpAddresses ipAddresses)
{
    SWSS_LOG_ENTER();

    assert(!hasNextHopGroup(ipAddresses));

    if (m_nextHopGroupCount >= m_maxNextHopGroupCount)
    {
        SWSS_LOG_DEBUG("Failed to create new next hop group. \
                        Reaching maximum number of next hop groups.");
        return false;
    }

    vector<sai_object_id_t> next_hop_ids;
    set<IpAddress> next_hop_set = ipAddresses.getIpAddresses();
    std::map<sai_object_id_t, IpAddress> nhopgroup_members_set;

    /* Assert each IP address exists in m_syncdNextHops table,
     * and add the corresponding next_hop_id to next_hop_ids. */
    for (auto it : next_hop_set)
    {
        if (!m_neighOrch->hasNextHop(it))
        {
            SWSS_LOG_INFO("Failed to get next hop %s in %s",
                    it.to_string().c_str(), ipAddresses.to_string().c_str());
            return false;
        }

        sai_object_id_t next_hop_id = m_neighOrch->getNextHopId(it);
        next_hop_ids.push_back(next_hop_id);
        nhopgroup_members_set[next_hop_id] = it;
    }

    sai_attribute_t nhg_attr;
    vector<sai_attribute_t> nhg_attrs;

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_ECMP;
    nhg_attrs.push_back(nhg_attr);

    sai_object_id_t next_hop_group_id;
    sai_status_t status = sai_next_hop_group_api->create_next_hop_group(&next_hop_group_id,
                                                                        gSwitchId,
                                                                        (uint32_t)nhg_attrs.size(),
                                                                        nhg_attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create next hop group %s, rv:%d",
                       ipAddresses.to_string().c_str(), status);
        return false;
    }

    m_nextHopGroupCount ++;
    SWSS_LOG_NOTICE("Create next hop group %s", ipAddresses.to_string().c_str());

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);

    NextHopGroupEntry next_hop_group_entry;
    next_hop_group_entry.next_hop_group_id = next_hop_group_id;

    for (auto nhid: next_hop_ids)
    {
        // skip next hop group member create for neighbor from down port
        if (m_neighOrch->isNextHopFlagSet(nhopgroup_members_set[nhid], NHFLAGS_IFDOWN)) {
            continue;
        }

        // Create a next hop group member
        vector<sai_attribute_t> nhgm_attrs;

        sai_attribute_t nhgm_attr;
        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
        nhgm_attr.value.oid = next_hop_group_id;
        nhgm_attrs.push_back(nhgm_attr);

        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
        nhgm_attr.value.oid = nhid;
        nhgm_attrs.push_back(nhgm_attr);

        sai_object_id_t next_hop_group_member_id;
        status = sai_next_hop_group_api->create_next_hop_group_member(&next_hop_group_member_id,
                                                                      gSwitchId,
                                                                      (uint32_t)nhgm_attrs.size(),
                                                                      nhgm_attrs.data());

        if (status != SAI_STATUS_SUCCESS)
        {
            // TODO: do we need to clean up?
            SWSS_LOG_ERROR("Failed to create next hop group %lx member %lx: %d\n",
                           next_hop_group_id, next_hop_group_member_id, status);
            return false;
        }

        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);

        // Save the membership into next hop structure
        next_hop_group_entry.nhopgroup_members[nhopgroup_members_set.find(nhid)->second] =
                                                                next_hop_group_member_id;
    }

    /* Increment the ref_count for the next hops used by the next hop group. */
    for (auto it : next_hop_set)
        m_neighOrch->increaseNextHopRefCount(it);

    /*
     * Initialize the next hop group structure with ref_count as 0. This
     * count will increase once the route is successfully syncd.
     */
    next_hop_group_entry.ref_count = 0;
    m_syncdNextHopGroups[ipAddresses] = next_hop_group_entry;


    return true;
}

bool RouteOrch::removeNextHopGroup(IpAddresses ipAddresses)
{
    SWSS_LOG_ENTER();

    sai_object_id_t next_hop_group_id;
    auto next_hop_group_entry = m_syncdNextHopGroups.find(ipAddresses);
    sai_status_t status;

    assert(next_hop_group_entry != m_syncdNextHopGroups.end());

    if (next_hop_group_entry->second.ref_count != 0)
    {
        return true;
    }

    next_hop_group_id = next_hop_group_entry->second.next_hop_group_id;
    SWSS_LOG_NOTICE("Delete next hop group %s", ipAddresses.to_string().c_str());

    for (auto nhop = next_hop_group_entry->second.nhopgroup_members.begin();
         nhop != next_hop_group_entry->second.nhopgroup_members.end();)
    {

        if (m_neighOrch->isNextHopFlagSet(nhop->first, NHFLAGS_IFDOWN))
        {
            SWSS_LOG_WARN("NHFLAGS_IFDOWN set for next hop group member %s with next_hop_id %lx",
                           nhop->first.to_string().c_str(), nhop->second);
            nhop = next_hop_group_entry->second.nhopgroup_members.erase(nhop);
            continue;
        }
        status = sai_next_hop_group_api->remove_next_hop_group_member(nhop->second);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove next hop group member %lx, rv:%d",
                           nhop->second, status);
            return false;
        }

        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
        nhop = next_hop_group_entry->second.nhopgroup_members.erase(nhop);
    }

    status = sai_next_hop_group_api->remove_next_hop_group(next_hop_group_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove next hop group %lx, rv:%d", next_hop_group_id, status);
        return false;
    }

    m_nextHopGroupCount --;
    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);

    set<IpAddress> ip_address_set = ipAddresses.getIpAddresses();
    for (auto it : ip_address_set)
    {
        m_neighOrch->decreaseNextHopRefCount(it);
    }
    m_syncdNextHopGroups.erase(ipAddresses);

    return true;
}

void RouteOrch::addTempRoute(IpPrefix ipPrefix, IpAddresses nextHops)
{
    SWSS_LOG_ENTER();

    auto next_hop_set = nextHops.getIpAddresses();

    /* Remove next hops that are not in m_syncdNextHops */
    for (auto it = next_hop_set.begin(); it != next_hop_set.end();)
    {
        if (!m_neighOrch->hasNextHop(*it))
        {
            SWSS_LOG_INFO("Failed to get next hop %s for %s",
                   (*it).to_string().c_str(), ipPrefix.to_string().c_str());
            it = next_hop_set.erase(it);
        }
        else
            it++;
    }

    /* Return if next_hop_set is empty */
    if (next_hop_set.empty())
        return;

    /* Randomly pick an address from the set */
    auto it = next_hop_set.begin();
    advance(it, rand() % next_hop_set.size());

    /* Set the route's temporary next hop to be the randomly picked one */
    IpAddresses tmp_next_hop((*it).to_string());
    addRoute(ipPrefix, tmp_next_hop);
}

bool RouteOrch::addRoute(IpPrefix ipPrefix, IpAddresses nextHops)
{
    SWSS_LOG_ENTER();

    /* next_hop_id indicates the next hop id or next hop group id of this route */
    sai_object_id_t next_hop_id;
    auto it_route = m_syncdRoutes.find(ipPrefix);

    /* The route is pointing to a next hop */
    if (nextHops.getSize() == 1)
    {
        IpAddress ip_address(nextHops.to_string());
        if (m_neighOrch->hasNextHop(ip_address))
        {
            next_hop_id = m_neighOrch->getNextHopId(ip_address);
        }
        else
        {
            SWSS_LOG_INFO("Failed to get next hop %s for %s",
                    nextHops.to_string().c_str(), ipPrefix.to_string().c_str());
            return false;
        }
    }
    /* The route is pointing to a next hop group */
    else
    {
        /* Check if there is already an existing next hop group */
        if (!hasNextHopGroup(nextHops))
        {
            /* Try to create a new next hop group */
            if (!addNextHopGroup(nextHops))
            {
                /* Failed to create the next hop group and check if a temporary route is needed */

                /* If the current next hop is part of the next hop group to sync,
                 * then return false and no need to add another temporary route. */
                if (it_route != m_syncdRoutes.end() && it_route->second.getSize() == 1)
                {
                    IpAddress ip_address(it_route->second.to_string());
                    if (nextHops.contains(ip_address))
                    {
                        return false;
                    }
                }

                /* Add a temporary route when a next hop group cannot be added,
                 * and there is no temporary route right now or the current temporary
                 * route is not pointing to a member of the next hop group to sync. */
                addTempRoute(ipPrefix, nextHops);
                /* Return false since the original route is not successfully added */
                return false;
            }
        }

        next_hop_id = m_syncdNextHopGroups[nextHops].next_hop_group_id;
    }

    /* Sync the route entry */
    sai_route_entry_t route_entry;
    route_entry.vr_id = gVirtualRouterId;
    route_entry.switch_id = gSwitchId;
    copy(route_entry.destination, ipPrefix);

    sai_attribute_t route_attr;

    /* If the prefix is not in m_syncdRoutes, then we need to create the route
     * for this prefix with the new next hop (group) id. If the prefix is already
     * in m_syncdRoutes, then we need to update the route with a new next hop
     * (group) id. The old next hop (group) is then not used and the reference
     * count will decrease by 1.
     */
    if (it_route == m_syncdRoutes.end())
    {
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        route_attr.value.oid = next_hop_id;

        /* Default SAI_ROUTE_ATTR_PACKET_ACTION is SAI_PACKET_ACTION_FORWARD */
        sai_status_t status = sai_route_api->create_route_entry(&route_entry, 1, &route_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create route %s with next hop(s) %s",
                    ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
            /* Clean up the newly created next hop group entry */
            if (nextHops.getSize() > 1)
            {
                removeNextHopGroup(nextHops);
            }
            return false;
        }

        if (route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
        }
        else
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
        }

        /* Increase the ref_count for the next hop (group) entry */
        increaseNextHopRefCount(nextHops);
        SWSS_LOG_INFO("Create route %s with next hop(s) %s",
                ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
    }
    else
    {
        sai_status_t status;

        /* Set the packet action to forward when there was no next hop (dropped) */
        if (it_route->second.getSize() == 0)
        {
            route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
            route_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;

            status = sai_route_api->set_route_entry_attribute(&route_entry, &route_attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set route %s with packet action forward, %d",
                               ipPrefix.to_string().c_str(), status);
                return false;
            }
        }

        route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        route_attr.value.oid = next_hop_id;

        /* Set the next hop ID to a new value */
        status = sai_route_api->set_route_entry_attribute(&route_entry, &route_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set route %s with next hop(s) %s",
                    ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
            return false;
        }

        /* Increase the ref_count for the next hop (group) entry */
        increaseNextHopRefCount(nextHops);

        decreaseNextHopRefCount(it_route->second);
        if (it_route->second.getSize() > 1
            && m_syncdNextHopGroups[it_route->second].ref_count == 0)
        {
            removeNextHopGroup(it_route->second);
        }
        SWSS_LOG_INFO("Set route %s with next hop(s) %s",
                ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
    }

    m_syncdRoutes[ipPrefix] = nextHops;

    notifyNextHopChangeObservers(ipPrefix, nextHops, true);
    return true;
}

bool RouteOrch::removeRoute(IpPrefix ipPrefix)
{
    SWSS_LOG_ENTER();

    sai_route_entry_t route_entry;
    route_entry.vr_id = gVirtualRouterId;
    route_entry.switch_id = gSwitchId;
    copy(route_entry.destination, ipPrefix);

    // set to blackhole for default route
    if (ipPrefix.isDefaultRoute())
    {
        sai_attribute_t attr;
        attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        attr.value.s32 = SAI_PACKET_ACTION_DROP;

        sai_status_t status = sai_route_api->set_route_entry_attribute(&route_entry, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set route %s packet action to drop, rv:%d",
                    ipPrefix.to_string().c_str(), status);
            return false;
        }

        SWSS_LOG_INFO("Set route %s packet action to drop", ipPrefix.to_string().c_str());

        attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        attr.value.oid = SAI_NULL_OBJECT_ID;

        status = sai_route_api->set_route_entry_attribute(&route_entry, &attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set route %s next hop ID to NULL, rv:%d",
                    ipPrefix.to_string().c_str(), status);
            return false;
        }

        SWSS_LOG_INFO("Set route %s next hop ID to NULL", ipPrefix.to_string().c_str());
    }
    else
    {
        sai_status_t status = sai_route_api->remove_route_entry(&route_entry);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove route prefix:%s\n", ipPrefix.to_string().c_str());
            return false;
        }

        if (route_entry.destination.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
        {
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
        }
        else
        {
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
        }

    }
    /* Remove next hop group entry if ref_count is zero */
    auto it_route = m_syncdRoutes.find(ipPrefix);
    if (it_route != m_syncdRoutes.end())
    {
        /*
         * Decrease the reference count only when the route is pointing to a next hop.
         * Decrease the reference count when the route is pointing to a next hop group,
         * and check wheather the reference count decreases to zero. If yes, then we need
         * to remove the next hop group.
         */
        decreaseNextHopRefCount(it_route->second);
        if (it_route->second.getSize() > 1
            && m_syncdNextHopGroups[it_route->second].ref_count == 0)
        {
            removeNextHopGroup(it_route->second);
        }
    }
    SWSS_LOG_INFO("Remove route %s with next hop(s) %s",
            ipPrefix.to_string().c_str(), it_route->second.to_string().c_str());

    if (ipPrefix.isDefaultRoute())
    {
        m_syncdRoutes[ipPrefix] = IpAddresses();

        /* Notify about default route next hop change */
        notifyNextHopChangeObservers(ipPrefix, m_syncdRoutes[ipPrefix], true);
    }
    else
    {
        m_syncdRoutes.erase(ipPrefix);

        /* Notify about the route next hop removal */
        notifyNextHopChangeObservers(ipPrefix, IpAddresses(), false);
    }
    return true;
}
