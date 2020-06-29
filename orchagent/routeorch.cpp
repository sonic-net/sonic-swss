#include <assert.h>
#include <inttypes.h>
#include <algorithm>
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

RouteOrch::RouteOrch(DBConnector *db, string tableName, SwitchOrch *switchOrch, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch, FgNhgOrch *fgNhgOrch) :
        gRouteBulker(sai_route_api),
        gNextHopGroupMemberBulker(sai_next_hop_group_api, gSwitchId),
        Orch(db, tableName, routeorch_pri),
        m_switchOrch(switchOrch),
        m_neighOrch(neighOrch),
        m_intfsOrch(intfsOrch),
        m_vrfOrch(vrfOrch),
        m_fgNhgOrch(fgNhgOrch),
	m_fgNhgOrch(fgNhgOrch),
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
    vector<FieldValueTuple> fvTuple;
    fvTuple.emplace_back("MAX_NEXTHOP_GROUP_COUNT", to_string(m_maxNextHopGroupCount));
    m_switchOrch->set_switch_capability(fvTuple);

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
    m_syncdRoutes[gVirtualRouterId][default_ip_prefix] = NextHopGroupKey();

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
    m_syncdRoutes[gVirtualRouterId][v6_default_ip_prefix] = NextHopGroupKey();

    SWSS_LOG_NOTICE("Create IPv6 default route with packet action drop");

    /* All the interfaces have the same MAC address and hence the same
     * auto-generated link-local ipv6 address with eui64 interface-id.
     * Hence add a single /128 route entry for the link-local interface
     * address pointing to the CPU port.
     */
    IpPrefix linklocal_prefix = getLinkLocalEui64Addr();

    addLinkLocalRouteToMe(gVirtualRouterId, linklocal_prefix);

    /* Add fe80::/10 subnet route to forward all link-local packets
     * destined to us, to CPU */
    IpPrefix default_link_local_prefix("fe80::/10");

    addLinkLocalRouteToMe(gVirtualRouterId, default_link_local_prefix);

    /* TODO: Add the link-local fe80::/10 route to cpu in every VRF created from
     * vrforch::addOperation. */
}

std::string RouteOrch::getLinkLocalEui64Addr(void)
{
    SWSS_LOG_ENTER();

    string        ip_prefix;
    const uint8_t *gmac = gMacAddress.getMac();

    uint8_t        eui64_interface_id[EUI64_INTF_ID_LEN];
    char           ipv6_ll_addr[INET6_ADDRSTRLEN] = {0};

    /* Link-local IPv6 address autogenerated by kernel with eui64 interface-id
     * derived from the MAC address of the host interface.
     */
    eui64_interface_id[0] = gmac[0] ^ 0x02;
    eui64_interface_id[1] = gmac[1];
    eui64_interface_id[2] = gmac[2];
    eui64_interface_id[3] = 0xff;
    eui64_interface_id[4] = 0xfe;
    eui64_interface_id[5] = gmac[3];
    eui64_interface_id[6] = gmac[4];
    eui64_interface_id[7] = gmac[5];

    snprintf(ipv6_ll_addr, INET6_ADDRSTRLEN, "fe80::%02x%02x:%02x%02x:%02x%02x:%02x%02x",
             eui64_interface_id[0], eui64_interface_id[1], eui64_interface_id[2],
             eui64_interface_id[3], eui64_interface_id[4], eui64_interface_id[5],
             eui64_interface_id[6], eui64_interface_id[7]);

    ip_prefix = string(ipv6_ll_addr);

    return ip_prefix;
}

void RouteOrch::addLinkLocalRouteToMe(sai_object_id_t vrf_id, IpPrefix linklocal_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = vrf_id;
    copy(unicast_route_entry.destination, linklocal_prefix);
    subnet(unicast_route_entry.destination, unicast_route_entry.destination);

    sai_attribute_t attr;
    vector<sai_attribute_t> attrs;

    attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
    attr.value.s32 = SAI_PACKET_ACTION_FORWARD;
    attrs.push_back(attr);

    Port cpu_port;
    gPortsOrch->getCpuPort(cpu_port);

    attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
    attr.value.oid = cpu_port.m_port_id;
    attrs.push_back(attr);

    sai_status_t status = sai_route_api->create_route_entry(&unicast_route_entry, (uint32_t)attrs.size(), attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create link local ipv6 route %s to cpu, rv:%d",
                       linklocal_prefix.getIp().to_string().c_str(), status);
        throw runtime_error("Failed to create link local ipv6 route to cpu.");
    }

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);

    SWSS_LOG_NOTICE("Created link local ipv6 route  %s to cpu", linklocal_prefix.to_string().c_str());
}

bool RouteOrch::hasNextHopGroup(const NextHopGroupKey& nexthops) const
{
    return m_syncdNextHopGroups.find(nexthops) != m_syncdNextHopGroups.end();
}

sai_object_id_t RouteOrch::getNextHopGroupId(const NextHopGroupKey& nexthops)
{
    assert(hasNextHopGroup(nexthops));
    return m_syncdNextHopGroups[nexthops].next_hop_group_id;
}

void RouteOrch::attach(Observer *observer, const IpAddress& dstAddr, sai_object_id_t vrf_id)
{
    SWSS_LOG_ENTER();

    Host host = std::make_pair(vrf_id, dstAddr);
    auto observerEntry = m_nextHopObservers.find(host);

    /* Create a new observer entry if no current observer is observing this
     * IP address */
    if (observerEntry == m_nextHopObservers.end())
    {
        m_nextHopObservers.emplace(host, NextHopObserverEntry());
        observerEntry = m_nextHopObservers.find(host);

        /* Find the prefixes that cover the destination IP */
        if (m_syncdRoutes.find(vrf_id) != m_syncdRoutes.end())
        {
            for (auto route : m_syncdRoutes.at(vrf_id))
            {
                if (route.first.isAddressInSubnet(dstAddr))
                {
                    SWSS_LOG_INFO("Prefix %s covers destination address",
                            route.first.to_string().c_str());
                    observerEntry->second.routeTable.emplace(
                            route.first, route.second);
                }
            }
        }
    }

    observerEntry->second.observers.push_back(observer);

    // Trigger next hop change for the first time the observer is attached
    // Note that rbegin() is pointing to the entry with longest prefix match
    auto route = observerEntry->second.routeTable.rbegin();
    if (route != observerEntry->second.routeTable.rend())
    {
        SWSS_LOG_NOTICE("Attached next hop observer of route %s for destination IP %s",
                observerEntry->second.routeTable.rbegin()->first.to_string().c_str(),
                dstAddr.to_string().c_str());
        NextHopUpdate update = { vrf_id, dstAddr, route->first, route->second };
        observer->update(SUBJECT_TYPE_NEXTHOP_CHANGE, static_cast<void *>(&update));
    }
}

void RouteOrch::detach(Observer *observer, const IpAddress& dstAddr, sai_object_id_t vrf_id)
{
    SWSS_LOG_ENTER();

    auto observerEntry = m_nextHopObservers.find(std::make_pair(vrf_id, dstAddr));

    if (observerEntry == m_nextHopObservers.end())
    {
        SWSS_LOG_ERROR("Failed to locate observer for destination IP %s",
                dstAddr.to_string().c_str());
        assert(false);
        return;
    }

    // Find the observer
    for (auto iter = observerEntry->second.observers.begin();
            iter != observerEntry->second.observers.end(); ++iter)
    {
        if (observer == *iter)
        {
            observerEntry->second.observers.erase(iter);

            SWSS_LOG_NOTICE("Detached next hop observer for destination IP %s",
                    dstAddr.to_string().c_str());

            // Remove NextHopObserverEntry if no observer is tracking this
            // destination IP.
            if (observerEntry->second.observers.empty())
            {
                m_nextHopObservers.erase(observerEntry);
            }
            break;
        }
    }
}

bool RouteOrch::validnexthopinNextHopGroup(const NextHopKey &nexthop)
{
    SWSS_LOG_ENTER();

    sai_object_id_t nexthop_id;
    sai_status_t status;

    for (auto nhopgroup = m_syncdNextHopGroups.begin();
         nhopgroup != m_syncdNextHopGroups.end(); ++nhopgroup)
    {

        if (!(nhopgroup->first.contains(nexthop)))
        {
            continue;
        }

        vector<sai_attribute_t> nhgm_attrs;
        sai_attribute_t nhgm_attr;

        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
        nhgm_attr.value.oid = nhopgroup->second.next_hop_group_id;
        nhgm_attrs.push_back(nhgm_attr);

        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
        nhgm_attr.value.oid = m_neighOrch->getNextHopId(nexthop);
        nhgm_attrs.push_back(nhgm_attr);

        status = sai_next_hop_group_api->create_next_hop_group_member(&nexthop_id, gSwitchId,
                                                                      (uint32_t)nhgm_attrs.size(),
                                                                      nhgm_attrs.data());

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to add next hop member to group %" PRIx64 ": %d\n",
                           nhopgroup->second.next_hop_group_id, status);
            return false;
        }

        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
        nhopgroup->second.nhopgroup_members[nexthop] = nexthop_id;
    }

    if(!m_fgNhgOrch->validnexthopinNextHopGroup(nexthop))
    {
        return false;
    }

    return true;
}

bool RouteOrch::invalidnexthopinNextHopGroup(const NextHopKey &nexthop)
{
    SWSS_LOG_ENTER();

    sai_object_id_t nexthop_id;
    sai_status_t status;

    for (auto nhopgroup = m_syncdNextHopGroups.begin();
         nhopgroup != m_syncdNextHopGroups.end(); ++nhopgroup)
    {

        if (!(nhopgroup->first.contains(nexthop)))
        {
            continue;
        }

        nexthop_id = nhopgroup->second.nhopgroup_members[nexthop];
        status = sai_next_hop_group_api->remove_next_hop_group_member(nexthop_id);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove next hop member %" PRIx64 " from group %" PRIx64 ": %d\n",
                           nexthop_id, nhopgroup->second.next_hop_group_id, status);
            return false;
        }

        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    if(!m_fgNhgOrch->invalidnexthopinNextHopGroup(nexthop))
    {
        return false;
    }

    return true;
}

void RouteOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        // Route bulk results will be stored in a map
        std::map<
                std::pair<
                        std::string,            // Key
                        std::string             // Op
                >,
                RouteBulkContext
        >                                       toBulk;

        // Add or remove routes with a route bulker
        while (it != consumer.m_toSync.end())
        {
            KeyOpFieldsValuesTuple t = it->second;

            string key = kfvKey(t);
            string op = kfvOp(t);

            auto rc = toBulk.emplace(std::piecewise_construct,
                    std::forward_as_tuple(key, op),
                    std::forward_as_tuple());

            bool inserted = rc.second;
            auto& ctx = rc.first->second;
            if (!inserted)
            {
                ctx.clear();
            }

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
                    for (auto j : m_syncdRoutes)
                    {
                        string vrf;

                        if (j.first != gVirtualRouterId)
                        {
                            vrf = m_vrfOrch->getVRFname(j.first) + ":";
                        }

                        for (auto i : j.second)
                        {
                            vector<FieldValueTuple> v;
                            key = vrf + i.first.to_string();
                            auto x = KeyOpFieldsValuesTuple(key, DEL_COMMAND, v);
                            consumer.addToSync(x);
                        }
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

            sai_object_id_t& vrf_id = ctx.vrf_id;
            IpPrefix& ip_prefix = ctx.ip_prefix;

            if (!key.compare(0, strlen(VRF_PREFIX), VRF_PREFIX))
            {
                size_t found = key.find(':');
                string vrf_name = key.substr(0, found);

                if (!m_vrfOrch->isVRFexists(vrf_name))
                {
                    it++;
                    continue;
                }
                vrf_id = m_vrfOrch->getVRFid(vrf_name);
                ip_prefix = IpPrefix(key.substr(found+1));
            }
            else
            {
                vrf_id = gVirtualRouterId;
                ip_prefix = IpPrefix(key);
            }

            if (op == SET_COMMAND)
            {
                string ips;
                string aliases;
                bool& excp_intfs_flag = ctx.excp_intfs_flag;

                for (auto i : kfvFieldsValues(t))
                {
                    if (fvField(i) == "nexthop")
                        ips = fvValue(i);

                    if (fvField(i) == "ifname")
                        aliases = fvValue(i);
                }
                vector<string>& ipv = ctx.ipv;
                ipv = tokenize(ips, ',');
                vector<string> alsv = tokenize(aliases, ',');

                /*
                 * For backward compatibility, adjust ip string from old format to
                 * new format. Meanwhile it can deal with some abnormal cases.
                 */

                /* Resize the ip vector to match ifname vector
                 * as tokenize(",", ',') will miss the last empty segment. */
                if (alsv.size() == 0)
                {
                    SWSS_LOG_WARN("Skip the route %s, for it has an empty ifname field.", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                else if (alsv.size() != ipv.size())
                {
                    SWSS_LOG_NOTICE("Route %s: resize ipv to match alsv, %zd -> %zd.", key.c_str(), ipv.size(), alsv.size());
                    ipv.resize(alsv.size());
                }

                /* Set the empty ip(s) to zero
                 * as IpAddress("") will construst a incorrect ip. */
                for (auto &ip : ipv)
                {
                    if (ip.empty())
                    {
                        SWSS_LOG_NOTICE("Route %s: set the empty nexthop ip to zero.", key.c_str());
                        ip = ip_prefix.isV4() ? "0.0.0.0" : "::";
                    }
                }

                for (auto alias : alsv)
                {
                    if (alias == "eth0" || alias == "lo" || alias == "docker0")
                    {
                        excp_intfs_flag = true;
                        break;
                    }
                }

                // TODO: cannot trust m_portsOrch->getPortIdByAlias because sometimes alias is empty
                if (excp_intfs_flag)
                {
                    /* If any existing routes are updated to point to the
                     * above interfaces, remove them from the ASIC. */
                    if (removeRoute(ctx))
                        it = consumer.m_toSync.erase(it);
                    else
                        it++;
                    continue;
                }

                string nhg_str = ipv[0] + NH_DELIMITER + alsv[0];
                for (uint32_t i = 1; i < ipv.size(); i++)
                {
                    nhg_str += NHG_DELIMITER + ipv[i] + NH_DELIMITER + alsv[i];
                }

                NextHopGroupKey& nhg = ctx.nhg;
                nhg = NextHopGroupKey(nhg_str);

                if (ipv.size() == 1 && IpAddress(ipv[0]).isZero())
                {
                    /* blackhole to be done */
                    if (alsv[0] == "unknown")
                    {
                        /* add addBlackholeRoute or addRoute support empty nhg */
                        it = consumer.m_toSync.erase(it);
                    }
                    /* directly connected route to VRF interface which come from kernel */
                    else if (!alsv[0].compare(0, strlen(VRF_PREFIX), VRF_PREFIX))
                    {
                        it = consumer.m_toSync.erase(it);
                    }
                    /* skip prefix which is linklocal or multicast */
                    else if (ip_prefix.getIp().getAddrScope() != IpAddress::GLOBAL_SCOPE)
                    {
                        it = consumer.m_toSync.erase(it);
                    }
                    /* fullmask subnet route is same as ip2me route */
                    else if (ip_prefix.isFullMask() && m_intfsOrch->isPrefixSubnet(ip_prefix, alsv[0]))
                    {
                        it = consumer.m_toSync.erase(it);
                    }
                    /* subnet route, vrf leaked route, etc */
                    else
                    {
                        if (addRoute(ctx, nhg))
                             it = consumer.m_toSync.erase(it);
                        else
                            it++;
                    }
                }
                else if (m_syncdRoutes.find(vrf_id) == m_syncdRoutes.end() ||
                    m_syncdRoutes.at(vrf_id).find(ip_prefix) == m_syncdRoutes.at(vrf_id).end() ||
                    m_syncdRoutes.at(vrf_id).at(ip_prefix) != nhg)
                {
                    if (addRoute(ctx, nhg))
                        it = consumer.m_toSync.erase(it);
                    else
                        it++;
                }
                else
                    /* Duplicate entry */
                    it = consumer.m_toSync.erase(it);

                // If already exhaust the nexthop groups, and there are pending removing routes in bulker,
                // flush the bulker and possibly collect some released nexthop groups
                if (m_nextHopGroupCount >= m_maxNextHopGroupCount && gRouteBulker.removing_entries_count() > 0)
                {
                    break;
                }
            }
            else if (op == DEL_COMMAND)
            {
                if (removeRoute(ctx))
                    it = consumer.m_toSync.erase(it);
                else
                    it++;
            }
            else
            {
                SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
                it = consumer.m_toSync.erase(it);
            }
        }

        // Flush the route bulker, so routes will be written to syncd and ASIC
        gRouteBulker.flush();

        // Go through the bulker results
        auto it_prev = consumer.m_toSync.begin();
        while (it_prev != it)
        {
            KeyOpFieldsValuesTuple t = it_prev->second;

            string key = kfvKey(t);
            string op = kfvOp(t);
            auto found = toBulk.find(make_pair(key, op));
            if (found == toBulk.end())
            {
                it_prev++;
                continue;
            }

            const auto& ctx = found->second;
            const auto& object_statuses = ctx.object_statuses;
            if (object_statuses.empty())
            {
                it_prev++;
                continue;
            }

            const sai_object_id_t& vrf_id = ctx.vrf_id;
            const IpPrefix& ip_prefix = ctx.ip_prefix;

            if (op == SET_COMMAND)
            {
                const bool& excp_intfs_flag = ctx.excp_intfs_flag;
                const vector<string>& ipv = ctx.ipv;

                if (excp_intfs_flag)
                {
                    /* If any existing routes are updated to point to the
                     * above interfaces, remove them from the ASIC. */
                    if (removeRoutePost(ctx))
                        it_prev = consumer.m_toSync.erase(it_prev);
                    else
                        it_prev++;
                    continue;
                }

                const NextHopGroupKey& nhg = ctx.nhg;

                if (ipv.size() == 1 && IpAddress(ipv[0]).isZero())
                {
                    if (addRoutePost(ctx, nhg))
                        it_prev = consumer.m_toSync.erase(it_prev);
                    else
                        it_prev++;
                }
                else if (m_syncdRoutes.find(vrf_id) == m_syncdRoutes.end() ||
                    m_syncdRoutes.at(vrf_id).find(ip_prefix) == m_syncdRoutes.at(vrf_id).end() ||
                    m_syncdRoutes.at(vrf_id).at(ip_prefix) != nhg)
                {
                    if (addRoutePost(ctx, nhg))
                        it_prev = consumer.m_toSync.erase(it_prev);
                    else
                        it_prev++;
                }
            }
            else if (op == DEL_COMMAND)
            {
                /* Cannot locate the route or remove succeed */
                if (removeRoutePost(ctx))
                    it_prev = consumer.m_toSync.erase(it_prev);
                else
                    it_prev++;
            }
        }
    }
}

void RouteOrch::notifyNextHopChangeObservers(sai_object_id_t vrf_id, const IpPrefix &prefix, const NextHopGroupKey &nexthops, bool add)
{
    SWSS_LOG_ENTER();

    for (auto& entry : m_nextHopObservers)
    {
        if (vrf_id != entry.first.first || !prefix.isAddressInSubnet(entry.first.second))
        {
            continue;
        }

        if (add)
        {
            bool update_required = false;
            NextHopUpdate update = { vrf_id, entry.first.second, prefix, nexthops };

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
                    NextHopUpdate update = { vrf_id, entry.first.second, route->first, route->second };

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

void RouteOrch::increaseNextHopRefCount(const NextHopGroupKey &nexthops)
{
    /* Return when there is no next hop (dropped) */
    if (nexthops.getSize() == 0)
    {
        return;
    }
    else if (nexthops.getSize() == 1)
    {
        NextHopKey nexthop(nexthops.to_string());
        if (nexthop.ip_address.isZero())
            m_intfsOrch->increaseRouterIntfsRefCount(nexthop.alias);
        else
            m_neighOrch->increaseNextHopRefCount(nexthop);
    }
    else
    {
        m_syncdNextHopGroups[nexthops].ref_count ++;
    }
}
void RouteOrch::decreaseNextHopRefCount(const NextHopGroupKey &nexthops)
{
    /* Return when there is no next hop (dropped) */
    if (nexthops.getSize() == 0)
    {
        return;
    }
    else if (nexthops.getSize() == 1)
    {
        NextHopKey nexthop(nexthops.to_string());
        if (nexthop.ip_address.isZero())
            m_intfsOrch->decreaseRouterIntfsRefCount(nexthop.alias);
        else
            m_neighOrch->decreaseNextHopRefCount(nexthop);
    }
    else
    {
        m_syncdNextHopGroups[nexthops].ref_count --;
    }
}

bool RouteOrch::isRefCounterZero(const NextHopGroupKey &nexthops) const
{
    if (!hasNextHopGroup(nexthops))
    {
        return true;
    }

    return m_syncdNextHopGroups.at(nexthops).ref_count == 0;
}

bool RouteOrch::addNextHopGroup(const NextHopGroupKey &nexthops)
{
    SWSS_LOG_ENTER();

    assert(!hasNextHopGroup(nexthops));

    if (m_nextHopGroupCount >= m_maxNextHopGroupCount)
    {
        SWSS_LOG_DEBUG("Failed to create new next hop group. \
                        Reaching maximum number of next hop groups.");
        return false;
    }

    vector<sai_object_id_t> next_hop_ids;
    set<NextHopKey> next_hop_set = nexthops.getNextHops();
    std::map<sai_object_id_t, NextHopKey> nhopgroup_members_set;

    /* Assert each IP address exists in m_syncdNextHops table,
     * and add the corresponding next_hop_id to next_hop_ids. */
    for (auto it : next_hop_set)
    {
        if (!m_neighOrch->hasNextHop(it))
        {
            SWSS_LOG_INFO("Failed to get next hop %s in %s",
                    it.to_string().c_str(), nexthops.to_string().c_str());
            return false;
        }

        // skip next hop group member create for neighbor from down port
        if (m_neighOrch->isNextHopFlagSet(it, NHFLAGS_IFDOWN))
        {
            continue;
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
                       nexthops.to_string().c_str(), status);
        return false;
    }

    m_nextHopGroupCount ++;
    SWSS_LOG_NOTICE("Create next hop group %s", nexthops.to_string().c_str());

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);

    NextHopGroupEntry next_hop_group_entry;
    next_hop_group_entry.next_hop_group_id = next_hop_group_id;

    size_t npid_count = next_hop_ids.size();
    vector<sai_object_id_t> nhgm_ids(npid_count);
    for (size_t i = 0; i < npid_count; i++)
    {
        auto nhid = next_hop_ids[i];

        // Create a next hop group member
        vector<sai_attribute_t> nhgm_attrs;

        sai_attribute_t nhgm_attr;
        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
        nhgm_attr.value.oid = next_hop_group_id;
        nhgm_attrs.push_back(nhgm_attr);

        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
        nhgm_attr.value.oid = nhid;
        nhgm_attrs.push_back(nhgm_attr);

        gNextHopGroupMemberBulker.create_entry(&nhgm_ids[i],
                                                 (uint32_t)nhgm_attrs.size(),
                                                 nhgm_attrs.data());
    }

    gNextHopGroupMemberBulker.flush();
    for (size_t i = 0; i < npid_count; i++)
    {
        auto nhid = next_hop_ids[i];
        auto nhgm_id = nhgm_ids[i];
        if (nhgm_id == SAI_NULL_OBJECT_ID)
        {
            // TODO: do we need to clean up?
            SWSS_LOG_ERROR("Failed to create next hop group %" PRIx64 " member %" PRIx64 ": %d\n",
                           next_hop_group_id, nhgm_ids[i], status);
            return false;
        }

        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);

        // Save the membership into next hop structure
        next_hop_group_entry.nhopgroup_members[nhopgroup_members_set.find(nhid)->second] =
                                                                nhgm_id;
    }

    /* Increment the ref_count for the next hops used by the next hop group. */
    for (auto it : next_hop_set)
        m_neighOrch->increaseNextHopRefCount(it);

    /*
     * Initialize the next hop group structure with ref_count as 0. This
     * count will increase once the route is successfully syncd.
     */
    next_hop_group_entry.ref_count = 0;
    m_syncdNextHopGroups[nexthops] = next_hop_group_entry;


    return true;
}

bool RouteOrch::removeNextHopGroup(const NextHopGroupKey &nexthops)
{
    SWSS_LOG_ENTER();

    sai_object_id_t next_hop_group_id;
    auto next_hop_group_entry = m_syncdNextHopGroups.find(nexthops);
    sai_status_t status;

    assert(next_hop_group_entry != m_syncdNextHopGroups.end());

    if (next_hop_group_entry->second.ref_count != 0)
    {
        return true;
    }

    next_hop_group_id = next_hop_group_entry->second.next_hop_group_id;
    SWSS_LOG_NOTICE("Delete next hop group %s", nexthops.to_string().c_str());

    vector<sai_object_id_t> next_hop_ids;
    auto& nhgm = next_hop_group_entry->second.nhopgroup_members;
    for (auto nhop = nhgm.begin(); nhop != nhgm.end();)
    {
        if (m_neighOrch->isNextHopFlagSet(nhop->first, NHFLAGS_IFDOWN))
        {
            SWSS_LOG_WARN("NHFLAGS_IFDOWN set for next hop group member %s with next_hop_id %" PRIx64,
                           nhop->first.to_string().c_str(), nhop->second);
            nhop = nhgm.erase(nhop);
            continue;
        }

        next_hop_ids.push_back(nhop->second);
        nhop = nhgm.erase(nhop);
    }

    size_t nhid_count = next_hop_ids.size();
    vector<sai_status_t> statuses(nhid_count);
    for (size_t i = 0; i < nhid_count; i++)
    {
        gNextHopGroupMemberBulker.remove_entry(&statuses[i], next_hop_ids[i]);
    }
    gNextHopGroupMemberBulker.flush();
    for (size_t i = 0; i < nhid_count; i++)
    {
        if (statuses[i] != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove next hop group member[%zu] %" PRIx64 ", rv:%d",
                           i, next_hop_ids[i], statuses[i]);
            return false;
        }

        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    status = sai_next_hop_group_api->remove_next_hop_group(next_hop_group_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove next hop group %" PRIx64 ", rv:%d", next_hop_group_id, status);
        return false;
    }

    m_nextHopGroupCount --;
    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);

    set<NextHopKey> next_hop_set = nexthops.getNextHops();
    for (auto it : next_hop_set)
    {
        m_neighOrch->decreaseNextHopRefCount(it);
    }
    m_syncdNextHopGroups.erase(nexthops);

    return true;
}

void RouteOrch::addTempRoute(RouteBulkContext& ctx, const NextHopGroupKey &nextHops)
{
    SWSS_LOG_ENTER();

    IpPrefix& ipPrefix = ctx.ip_prefix;

    auto next_hop_set = nextHops.getNextHops();

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
    NextHopGroupKey tmp_next_hop((*it).to_string());
    ctx.tmp_next_hop = tmp_next_hop;

    addRoute(ctx, tmp_next_hop);
}

bool RouteOrch::addRoute(RouteBulkContext& ctx, const NextHopGroupKey &nextHops)
{
    SWSS_LOG_ENTER();

    sai_object_id_t& vrf_id = ctx.vrf_id;
    IpPrefix& ipPrefix = ctx.ip_prefix;

    if(m_fgNhgOrch->fgNhgPrefixes.find(ipPrefix) != m_fgNhgOrch->fgNhgPrefixes.end()
            && vrf_id == gVirtualRouterId){
        /* Only support the default vrf for Fine Grained ECMP */
        SWSS_LOG_INFO("Reroute %s:%s to fgNhgOrch", ipPrefix.to_string().c_str(), 
                nextHops.to_string().c_str());
        return m_fgNhgOrch->addRoute(vrf_id, ipPrefix, nextHops);
    }

    /* next_hop_id indicates the next hop id or next hop group id of this route */
    sai_object_id_t next_hop_id;

    if (m_syncdRoutes.find(vrf_id) == m_syncdRoutes.end())
    {
        m_syncdRoutes.emplace(vrf_id, RouteTable());
        m_vrfOrch->increaseVrfRefCount(vrf_id);
    }

    auto it_route = m_syncdRoutes.at(vrf_id).find(ipPrefix);

    /* The route is pointing to a next hop */
    if (nextHops.getSize() == 1)
    {
        NextHopKey nexthop(nextHops.to_string());
        if (nexthop.ip_address.isZero())
        {
            next_hop_id = m_intfsOrch->getRouterIntfsId(nexthop.alias);
            /* rif is not created yet */
            if (next_hop_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_INFO("Failed to get next hop %s for %s",
                        nextHops.to_string().c_str(), ipPrefix.to_string().c_str());
                return false;
            }
        }
        else
        {
            if (m_neighOrch->hasNextHop(nexthop))
            {
                next_hop_id = m_neighOrch->getNextHopId(nexthop);
            }
            else
            {
                SWSS_LOG_INFO("Failed to get next hop %s for %s",
                        nextHops.to_string().c_str(), ipPrefix.to_string().c_str());
                return false;
            }
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
                if (it_route != m_syncdRoutes.at(vrf_id).end() && it_route->second.getSize() == 1)
                {
                    NextHopKey nexthop(it_route->second.to_string());
                    if (nextHops.contains(nexthop))
                    {
                        return false;
                    }
                }

                /* Add a temporary route when a next hop group cannot be added,
                 * and there is no temporary route right now or the current temporary
                 * route is not pointing to a member of the next hop group to sync. */
                addTempRoute(ctx, nextHops);
                /* Return false since the original route is not successfully added */
                return false;
            }
        }

        next_hop_id = m_syncdNextHopGroups[nextHops].next_hop_group_id;
    }

    /* Sync the route entry */
    sai_route_entry_t route_entry;
    route_entry.vr_id = vrf_id;
    route_entry.switch_id = gSwitchId;
    copy(route_entry.destination, ipPrefix);

    sai_attribute_t route_attr;
    auto& object_statuses = ctx.object_statuses;

    /* If the prefix is not in m_syncdRoutes, then we need to create the route
     * for this prefix with the new next hop (group) id. If the prefix is already
     * in m_syncdRoutes, then we need to update the route with a new next hop
     * (group) id. The old next hop (group) is then not used and the reference
     * count will decrease by 1.
     */
    if (it_route == m_syncdRoutes.at(vrf_id).end())
    {
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        route_attr.value.oid = next_hop_id;

        /* Default SAI_ROUTE_ATTR_PACKET_ACTION is SAI_PACKET_ACTION_FORWARD */
        object_statuses.emplace_back();
        sai_status_t status = gRouteBulker.create_entry(&object_statuses.back(), &route_entry, 1, &route_attr);
        if (status == SAI_STATUS_ITEM_ALREADY_EXISTS)
        {
            SWSS_LOG_ERROR("Failed to create route %s with next hop(s) %s: already exists in bulker",
                    ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
            return false;
        }
    }
    else
    {
        /* Set the packet action to forward when there was no next hop (dropped) */
        if (it_route->second.getSize() == 0)
        {
            route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
            route_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;

            object_statuses.emplace_back();
            gRouteBulker.set_entry_attribute(&object_statuses.back(), &route_entry, &route_attr);
        }

        route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        route_attr.value.oid = next_hop_id;

        /* Set the next hop ID to a new value */
        object_statuses.emplace_back();
        gRouteBulker.set_entry_attribute(&object_statuses.back(), &route_entry, &route_attr);
    }
    return false;
}

bool RouteOrch::addRoutePost(const RouteBulkContext& ctx, const NextHopGroupKey &nextHops)
{
    SWSS_LOG_ENTER();

    const sai_object_id_t& vrf_id = ctx.vrf_id;
    const IpPrefix& ipPrefix = ctx.ip_prefix;

    const auto& object_statuses = ctx.object_statuses;

    if (object_statuses.empty())
    {
        // Something went wrong before router bulker, will retry
        return false;
    }

    /* next_hop_id indicates the next hop id or next hop group id of this route */
    sai_object_id_t next_hop_id;

    /* The route is pointing to a next hop */
    if (nextHops.getSize() == 1)
    {
        NextHopKey nexthop(nextHops.to_string());
        if (nexthop.ip_address.isZero())
        {
            next_hop_id = m_intfsOrch->getRouterIntfsId(nexthop.alias);
            /* rif is not created yet */
            if (next_hop_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_INFO("Failed to get next hop %s for %s",
                        nextHops.to_string().c_str(), ipPrefix.to_string().c_str());
                return false;
            }
        }
        else
        {
            if (!m_neighOrch->hasNextHop(nexthop))
            {
                SWSS_LOG_INFO("Failed to get next hop %s for %s",
                        nextHops.to_string().c_str(), ipPrefix.to_string().c_str());
                return false;
            }
        }
    }
    /* The route is pointing to a next hop group */
    else
    {
        if (!hasNextHopGroup(nextHops))
        {
            // Previous added an temporary route
            auto& tmp_next_hop = ctx.tmp_next_hop;
            addRoutePost(ctx, tmp_next_hop);
            return false;
        }
    }

    auto it_status = object_statuses.begin();
    auto it_route = m_syncdRoutes.at(vrf_id).find(ipPrefix);
    if (it_route == m_syncdRoutes.at(vrf_id).end())
    {
        if (*it_status++ != SAI_STATUS_SUCCESS)
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

        if (ipPrefix.isV4())
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
        }
        else
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
        }

        /* Increase the ref_count for the next hop (group) entry */
        increaseNextHopRefCount(nextHops);
        SWSS_LOG_INFO("Post create route %s with next hop(s) %s",
                ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
    }
    else
    {
        sai_status_t status;

        /* Set the packet action to forward when there was no next hop (dropped) */
        if (it_route->second.getSize() == 0)
        {
            status = *it_status++;
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set route %s with packet action forward, %d",
                               ipPrefix.to_string().c_str(), status);
                return false;
            }
        }

        status = *it_status++;
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
        SWSS_LOG_INFO("Post set route %s with next hop(s) %s",
                ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
    }

    m_syncdRoutes[vrf_id][ipPrefix] = nextHops;

    notifyNextHopChangeObservers(vrf_id, ipPrefix, nextHops, true);
    return true;
}

bool RouteOrch::removeRoute(RouteBulkContext& ctx)
{
    SWSS_LOG_ENTER();

    sai_object_id_t& vrf_id = ctx.vrf_id;
    IpPrefix& ipPrefix = ctx.ip_prefix;

    if(m_fgNhgOrch->fgNhgPrefixes.find(ipPrefix) != m_fgNhgOrch->fgNhgPrefixes.end()
            && vrf_id == gVirtualRouterId){
        /* Only support the default vrf for Fine Grained ECMP */
        SWSS_LOG_INFO("Reroute %s to fgNhgOrch", ipPrefix.to_string().c_str());
        return m_fgNhgOrch->removeRoute(vrf_id, ipPrefix);
    }

    auto it_route_table = m_syncdRoutes.find(vrf_id);
    if (it_route_table == m_syncdRoutes.end())
    {
        SWSS_LOG_INFO("Failed to find route table, vrf_id 0x%" PRIx64 "\n", vrf_id);
        return true;
    }

    sai_route_entry_t route_entry;
    route_entry.vr_id = vrf_id;
    route_entry.switch_id = gSwitchId;
    copy(route_entry.destination, ipPrefix);

    auto it_route = it_route_table->second.find(ipPrefix);
    size_t creating = gRouteBulker.creating_entries_count(route_entry);
    if (it_route == it_route_table->second.end() && creating == 0)
    {
        SWSS_LOG_INFO("Failed to find route entry, vrf_id 0x%" PRIx64 ", prefix %s\n", vrf_id,
                ipPrefix.to_string().c_str());
        return true;
    }

    auto& object_statuses = ctx.object_statuses;

    // set to blackhole for default route
    if (ipPrefix.isDefaultRoute())
    {
        sai_attribute_t attr;
        attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
        attr.value.s32 = SAI_PACKET_ACTION_DROP;

        object_statuses.emplace_back();
        gRouteBulker.set_entry_attribute(&object_statuses.back(), &route_entry, &attr);

        attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        attr.value.oid = SAI_NULL_OBJECT_ID;

        object_statuses.emplace_back();
        gRouteBulker.set_entry_attribute(&object_statuses.back(), &route_entry, &attr);
    }
    else
    {
        object_statuses.emplace_back();
        gRouteBulker.remove_entry(&object_statuses.back(), &route_entry);
    }

    return false;
}

bool RouteOrch::removeRoutePost(const RouteBulkContext& ctx)
{
    SWSS_LOG_ENTER();

    const sai_object_id_t& vrf_id = ctx.vrf_id;
    const IpPrefix& ipPrefix = ctx.ip_prefix;

    auto& object_statuses = ctx.object_statuses;

    if (object_statuses.empty())
    {
        // Something went wrong before router bulker, will retry
        return false;
    }

    auto it_route_table = m_syncdRoutes.find(vrf_id);
    auto it_route = it_route_table->second.find(ipPrefix);
    auto it_status = object_statuses.begin();

    // set to blackhole for default route
    if (ipPrefix.isDefaultRoute())
    {
        sai_status_t status = *it_status++;
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set route %s packet action to drop, rv:%d",
                    ipPrefix.to_string().c_str(), status);
            return false;
        }

        SWSS_LOG_INFO("Set route %s packet action to drop", ipPrefix.to_string().c_str());

        status = *it_status++;
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
        sai_status_t status = *it_status++;
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove route prefix:%s\n", ipPrefix.to_string().c_str());
            return false;
        }

        if (ipPrefix.isV4())
        {
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
        }
        else
        {
            gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
        }
    }

    /*
     * Decrease the reference count only when the route is pointing to a next hop.
     * Decrease the reference count when the route is pointing to a next hop group,
     * and check whether the reference count decreases to zero. If yes, then we need
     * to remove the next hop group.
     */
    decreaseNextHopRefCount(it_route->second);
    if (it_route->second.getSize() > 1
        && m_syncdNextHopGroups[it_route->second].ref_count == 0)
    {
        removeNextHopGroup(it_route->second);
    }

    SWSS_LOG_INFO("Remove route %s with next hop(s) %s",
            ipPrefix.to_string().c_str(), it_route->second.to_string().c_str());

    if (ipPrefix.isDefaultRoute())
    {
        it_route_table->second[ipPrefix] = NextHopGroupKey();

        /* Notify about default route next hop change */
        notifyNextHopChangeObservers(vrf_id, ipPrefix, it_route_table->second[ipPrefix], true);
    }
    else
    {
        it_route_table->second.erase(ipPrefix);

        /* Notify about the route next hop removal */
        notifyNextHopChangeObservers(vrf_id, ipPrefix, NextHopGroupKey(), false);

        if (it_route_table->second.size() == 0)
        {
            m_syncdRoutes.erase(vrf_id);
            m_vrfOrch->decreaseVrfRefCount(vrf_id);
        }
    }

    return true;
}
