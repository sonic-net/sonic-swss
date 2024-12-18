#include <assert.h>
#include <inttypes.h>
#include <algorithm>
#include "routeorch.h"
#include "nhgorch.h"
#include "tunneldecaporch.h"
#include "cbf/cbfnhgorch.h"
#include "logger.h"
#include "flowcounterrouteorch.h"
#include "muxorch.h"
#include "swssnet.h"
#include "crmorch.h"
#include "directory.h"

extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gSwitchId;

extern sai_next_hop_group_api_t*    sai_next_hop_group_api;
extern sai_route_api_t*             sai_route_api;
extern sai_mpls_api_t*              sai_mpls_api;
extern sai_switch_api_t*            sai_switch_api;

extern PortsOrch *gPortsOrch;
extern CrmOrch *gCrmOrch;
extern Directory<Orch*> gDirectory;
extern NhgOrch *gNhgOrch;
extern CbfNhgOrch *gCbfNhgOrch;
extern FlowCounterRouteOrch *gFlowCounterRouteOrch;
extern TunnelDecapOrch *gTunneldecapOrch;

extern size_t gMaxBulkSize;
extern string gMySwitchType;

/* Default maximum number of next hop groups */
#define DEFAULT_NUMBER_OF_ECMP_GROUPS   128
#define DEFAULT_MAX_ECMP_GROUP_SIZE     32

RouteOrch::RouteOrch(DBConnector *db, vector<table_name_with_pri_t> &tableNames, SwitchOrch *switchOrch, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch, FgNhgOrch *fgNhgOrch, Srv6Orch *srv6Orch) :
        gRouteBulker(sai_route_api, gMaxBulkSize),
        gLabelRouteBulker(sai_mpls_api, gMaxBulkSize),
        gNextHopGroupMemberBulker(sai_next_hop_group_api, gSwitchId, gMaxBulkSize),
        Orch(db, tableNames),
        m_switchOrch(switchOrch),
        m_neighOrch(neighOrch),
        m_intfsOrch(intfsOrch),
        m_vrfOrch(vrfOrch),
        m_fgNhgOrch(fgNhgOrch),
        m_nextHopGroupCount(0),
        m_srv6Orch(srv6Orch),
        m_resync(false),
        m_appTunnelDecapTermProducer(db, APP_TUNNEL_DECAP_TERM_TABLE_NAME)
{
    SWSS_LOG_ENTER();

    m_publisher.setBuffered(true);

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
         * according to different ECMP mode used.
         *
         * On Mellanox platform, the maximum ECMP groups returned is the value
         * under the condition that the ECMP group size is 1. Dividing this
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

    /* fetch the MAX_ECMP_MEMBER_COUNT and for voq platform, set it to 128 */
    attr.id = SAI_SWITCH_ATTR_MAX_ECMP_MEMBER_COUNT;
    status = sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr);
    
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Failed to get switch attribute max ECMP Group size. rv:%d", status);
    }
    else
    {
        uint32_t maxEcmpGroupSize = attr.value.u32;
        SWSS_LOG_NOTICE("Switch Type: %s, Max ECMP Group Size supported: %d", gMySwitchType.c_str(), attr.value.u32);

        /*If the switch type is voq, and max Ecmp group size supported is greater or equal to 128, set it to 128 */
        if (gMySwitchType == "voq" && maxEcmpGroupSize >= 128)
        {
            maxEcmpGroupSize = 128;
            attr.id = SAI_SWITCH_ATTR_ECMP_MEMBER_COUNT;
            attr.value.s32 = maxEcmpGroupSize;
            status = sai_switch_api->set_switch_attribute(gSwitchId, &attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set switch attribute ECMP member count to 128. rv:%d", status);
            }
            else 
            {
                SWSS_LOG_NOTICE("Set switch attribute ECMP member count to 128");
            }
        }
    }

    m_stateDb = shared_ptr<DBConnector>(new DBConnector("STATE_DB", 0));
    m_stateDefaultRouteTb = unique_ptr<swss::Table>(new Table(m_stateDb.get(), STATE_ROUTE_TABLE_NAME));

    IpPrefix default_ip_prefix("0.0.0.0/0");
    updateDefRouteState("0.0.0.0/0");

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
    m_syncdRoutes[gVirtualRouterId][default_ip_prefix] = RouteNhg();

    SWSS_LOG_NOTICE("Create IPv4 default route with packet action drop");

    IpPrefix v6_default_ip_prefix("::/0");
    updateDefRouteState("::/0");

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
    m_syncdRoutes[gVirtualRouterId][v6_default_ip_prefix] = RouteNhg();

    SWSS_LOG_NOTICE("Create IPv6 default route with packet action drop");

    /* All the interfaces have the same MAC address and hence the same
     * auto-generated link-local ipv6 address with eui64 interface-id.
     * Hence add a single /128 route entry for the link-local interface
     * address pointing to the CPU port.
     */
    IpPrefix linklocal_prefix = getLinkLocalEui64Addr();

    addLinkLocalRouteToMe(gVirtualRouterId, linklocal_prefix);
    SWSS_LOG_NOTICE("Created link local ipv6 route %s to cpu", linklocal_prefix.to_string().c_str());

    /* Add fe80::/10 subnet route to forward all link-local packets
     * destined to us, to CPU */
    IpPrefix default_link_local_prefix("fe80::/10");

    addLinkLocalRouteToMe(gVirtualRouterId, default_link_local_prefix);
    SWSS_LOG_NOTICE("Created link local ipv6 route %s to cpu", default_link_local_prefix.to_string().c_str());
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

    gFlowCounterRouteOrch->onAddMiscRouteEntry(vrf_id, linklocal_prefix.getSubnet());

    SWSS_LOG_NOTICE("Created link local ipv6 route  %s to cpu", linklocal_prefix.to_string().c_str());
}

void RouteOrch::delLinkLocalRouteToMe(sai_object_id_t vrf_id, IpPrefix linklocal_prefix)
{
    sai_route_entry_t unicast_route_entry;
    unicast_route_entry.switch_id = gSwitchId;
    unicast_route_entry.vr_id = vrf_id;
    copy(unicast_route_entry.destination, linklocal_prefix);
    subnet(unicast_route_entry.destination, unicast_route_entry.destination);

    sai_status_t status = sai_route_api->remove_route_entry(&unicast_route_entry);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to delete link local ipv6 route %s to cpu, rv:%d",
                       linklocal_prefix.getIp().to_string().c_str(), status);
        return;
    }

    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);

    gFlowCounterRouteOrch->onRemoveMiscRouteEntry(vrf_id, linklocal_prefix.getSubnet());

    SWSS_LOG_NOTICE("Deleted link local ipv6 route  %s to cpu", linklocal_prefix.to_string().c_str());
}

void RouteOrch::updateDefRouteState(string ip, bool add)
{
    vector<FieldValueTuple> tuples;
    string state = add?"ok":"na";
    FieldValueTuple tuple("state", state);
    tuples.push_back(tuple);

    m_stateDefaultRouteTb->set(ip, tuples);
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
        NextHopUpdate update = { vrf_id, dstAddr, route->first, route->second.nhg_key };
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

bool RouteOrch::validnexthopinNextHopGroup(const NextHopKey &nexthop, uint32_t& count)
{
    SWSS_LOG_ENTER();

    sai_object_id_t nexthop_id;
    sai_status_t status;
    count = 0;

    for (auto nhopgroup = m_syncdNextHopGroups.begin();
         nhopgroup != m_syncdNextHopGroups.end(); ++nhopgroup)
    {

        if (!(nhopgroup->first.contains(nexthop)))
        {
            continue;
        }

        vector<sai_attribute_t> nhgm_attrs;
        sai_attribute_t nhgm_attr;

        /* get updated nhkey with possible weight */
        auto nhkey = nhopgroup->first.getNextHops().find(nexthop);

        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
        nhgm_attr.value.oid = nhopgroup->second.next_hop_group_id;
        nhgm_attrs.push_back(nhgm_attr);

        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
        nhgm_attr.value.oid = m_neighOrch->getNextHopId(nexthop);
        nhgm_attrs.push_back(nhgm_attr);

        if (nhkey->weight)
        {
            nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT;
            nhgm_attr.value.s32 = nhkey->weight;
            nhgm_attrs.push_back(nhgm_attr);
        }

        if (m_switchOrch->checkOrderedEcmpEnable())
        {
            nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID;
            nhgm_attr.value.u32 = nhopgroup->second.nhopgroup_members[nexthop].seq_id;
            nhgm_attrs.push_back(nhgm_attr);
        }

        status = sai_next_hop_group_api->create_next_hop_group_member(&nexthop_id, gSwitchId,
                                                                      (uint32_t)nhgm_attrs.size(),
                                                                      nhgm_attrs.data());

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to add next hop member to group %" PRIx64 ": %d\n",
                           nhopgroup->second.next_hop_group_id, status);
            task_process_status handle_status = handleSaiCreateStatus(SAI_API_NEXT_HOP_GROUP, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        ++count;
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
        nhopgroup->second.nhopgroup_members[nexthop].next_hop_id = nexthop_id;
    }

    if (!m_fgNhgOrch->validNextHopInNextHopGroup(nexthop))
    {
        return false;
    }

    return true;
}

bool RouteOrch::invalidnexthopinNextHopGroup(const NextHopKey &nexthop, uint32_t& count)
{
    SWSS_LOG_ENTER();

    sai_object_id_t nexthop_id;
    sai_status_t status;
    count = 0;

    for (auto nhopgroup = m_syncdNextHopGroups.begin();
         nhopgroup != m_syncdNextHopGroups.end(); ++nhopgroup)
    {

        if (!(nhopgroup->first.contains(nexthop)))
        {
            continue;
        }

        nexthop_id = nhopgroup->second.nhopgroup_members[nexthop].next_hop_id;
        status = sai_next_hop_group_api->remove_next_hop_group_member(nexthop_id);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove next hop member %" PRIx64 " from group %" PRIx64 ": %d\n",
                           nexthop_id, nhopgroup->second.next_hop_group_id, status);
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEXT_HOP_GROUP, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        ++count;
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    if (!m_fgNhgOrch->invalidNextHopInNextHopGroup(nexthop))
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

    string table_name = consumer.getTableName();

    if (table_name == APP_LABEL_ROUTE_TABLE_NAME)
    {
        doLabelTask(consumer);
        return;
    }

    /* Default handling is for APP_ROUTE_TABLE_NAME */
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
                    std::forward_as_tuple(key, (op == SET_COMMAND)));

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
                string mpls_nhs;
                string vni_labels;
                string remote_macs;
                string weights;
                string nhg_index;
                bool& excp_intfs_flag = ctx.excp_intfs_flag;
                bool overlay_nh = false;
                bool blackhole = false;
                string srv6_segments;
                string srv6_source;
                bool srv6_nh = false;

                for (auto i : kfvFieldsValues(t))
                {
                    if (fvField(i) == "nexthop")
                        ips = fvValue(i);

                    if (fvField(i) == "ifname")
                        aliases = fvValue(i);

                    if (fvField(i) == "mpls_nh")
                        mpls_nhs = fvValue(i);

                    if (fvField(i) == "vni_label") {
                        vni_labels = fvValue(i);
                        overlay_nh = true;
                    }

                    if (fvField(i) == "router_mac")
                        remote_macs = fvValue(i);

                    if (fvField(i) == "blackhole")
                        blackhole = fvValue(i) == "true";

                    if (fvField(i) == "weight")
                        weights = fvValue(i);

                    if (fvField(i) == "nexthop_group")
                        nhg_index = fvValue(i);

                    if (fvField(i) == "segment") {
                        srv6_segments = fvValue(i);
                        srv6_nh = true;
                    }

                    if (fvField(i) == "seg_src")
                        srv6_source = fvValue(i);

                    if (fvField(i) == "protocol")
                    {
                        ctx.protocol = fvValue(i);
                    }
                }

                /*
                 * A route should not fill both nexthop_group and ips /
                 * aliases.
                 */
                if (!nhg_index.empty() && (!ips.empty() || !aliases.empty()))
                {
                    SWSS_LOG_ERROR("Route %s has both nexthop_group and ips/aliases", key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                ctx.nhg_index = nhg_index;

                /*
                 * If the nexthop_group is empty, create the next hop group key
                 * based on the IPs and aliases.  Otherwise, get the key from
                 * the NhgOrch.
                 */
                vector<string> ipv;
                vector<string> alsv;
                vector<string> mpls_nhv;
                vector<string> vni_labelv;
                vector<string> rmacv;
                NextHopGroupKey& nhg = ctx.nhg;
                vector<string> srv6_segv;
                vector<string> srv6_src;
                bool l3Vni = true;
                uint32_t vni = 0;

                /* Check if the next hop group is owned by the NhgOrch. */
                if (nhg_index.empty())
                {
                    ipv = tokenize(ips, ',');
                    alsv = tokenize(aliases, ',');
                    mpls_nhv = tokenize(mpls_nhs, ',');
                    vni_labelv = tokenize(vni_labels, ',');
                    rmacv = tokenize(remote_macs, ',');
                    srv6_segv = tokenize(srv6_segments, ',');
                    srv6_src = tokenize(srv6_source, ',');

                    /*
                    * For backward compatibility, adjust ip string from old format to
                    * new format. Meanwhile it can deal with some abnormal cases.
                    */

                    /* Resize the ip vector to match ifname vector
                    * as tokenize(",", ',') will miss the last empty segment. */
                    if (alsv.size() == 0 && !blackhole && !srv6_nh)
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

                    for (auto &vni_str: vni_labelv)
                    {
                        vni = static_cast<uint32_t>(std::stoul(vni_str));
                        if (!m_vrfOrch->isL3VniVlan(vni))
                        {
                            SWSS_LOG_WARN("Route %s is received on non L3 VNI %s", key.c_str(), vni_str.c_str());
                            l3Vni = false;
                            break;
                        }
                    }

                    if (!l3Vni)
                    {
                        it++;
                        continue;
                    }

                    /* Set the empty ip(s) to zero
                     * as IpAddress("") will construct a incorrect ip. */
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
                        /* skip route to management, docker, loopback
                        * TODO: for route to loopback interface, the proper
                        * way is to create loopback interface and then create
                        * route pointing to it, so that we can traps packets to
                        * CPU */
                        if (alias == "eth0" || alias == "docker0" ||
                            alias == "lo" || !alias.compare(0, strlen(LOOPBACK_PREFIX), LOOPBACK_PREFIX))
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

                        /* Publish route state to advertise routes to Loopback interface */
                        publishRouteState(ctx);
                        continue;
                    }

                    string nhg_str = "";

                    if (blackhole)
                    {
                        nhg = NextHopGroupKey();
                    }
                    else if (srv6_nh == true)
                    {
                        string ip;
                        if (ipv.empty())
                        {
                            ip = "0.0.0.0";
                        }
                        else
                        {
                            SWSS_LOG_ERROR("For SRV6 nexthop ipv should be empty");
                            it = consumer.m_toSync.erase(it);
                            continue;
                        }
                        nhg_str = ip + NH_DELIMITER + srv6_segv[0] + NH_DELIMITER + srv6_src[0];

                        for (uint32_t i = 1; i < srv6_segv.size(); i++)
                        {
                            nhg_str += NHG_DELIMITER + ip;
                            nhg_str += NH_DELIMITER + srv6_segv[i];
                            nhg_str += NH_DELIMITER + srv6_src[i];
                        }
                        nhg = NextHopGroupKey(nhg_str, overlay_nh, srv6_nh);
                        SWSS_LOG_INFO("SRV6 route with nhg %s", nhg.to_string().c_str());
                    }
                    else if (overlay_nh == false)
                    {
                        for (uint32_t i = 0; i < ipv.size(); i++)
                        {
                            if (i) nhg_str += NHG_DELIMITER;
                            if (alsv[i] == "tun0" && !(IpAddress(ipv[i]).isZero()))
                            {
                                alsv[i] = gIntfsOrch->getRouterIntfsAlias(ipv[i]);
                            }
                            if (!mpls_nhv.empty() && mpls_nhv[i] != "na")
                            {
                                nhg_str += mpls_nhv[i] + LABELSTACK_DELIMITER;
                            }
                            nhg_str += ipv[i] + NH_DELIMITER + alsv[i];
                        }

                        nhg = NextHopGroupKey(nhg_str, weights);
                    }
                    else
                    {
                        if(ipv.size() != rmacv.size()){
                            SWSS_LOG_ERROR("Skip route %s, it has an invalid router mac field %s", key.c_str(), remote_macs.c_str());
                            it = consumer.m_toSync.erase(it);
                            continue;
                        }

                        if(ipv.size() != vni_labelv.size()){
                            SWSS_LOG_ERROR("Skip route %s, it has an invalid vni label field %s", key.c_str(), vni_labels.c_str());
                            it = consumer.m_toSync.erase(it);
                            continue;
                        }

                        for (uint32_t i = 0; i < ipv.size(); i++)
                        {
                            if (i) nhg_str += NHG_DELIMITER;
                            nhg_str += ipv[i] + NH_DELIMITER + "vni" + alsv[i] + NH_DELIMITER + vni_labelv[i] + NH_DELIMITER + rmacv[i];
                        }

                        nhg = NextHopGroupKey(nhg_str, overlay_nh, srv6_nh);
                    }
                }
                else
                {
                    try
                    {
                        const NhgBase& nh_group = getNhg(nhg_index);
                        nhg = nh_group.getNhgKey();
                        ctx.using_temp_nhg = nh_group.isTemp();
                    }
                    catch (const std::out_of_range& e)
                    {
                        SWSS_LOG_ERROR("Next hop group %s does not exist", nhg_index.c_str());
                        ++it;
                        continue;
                    }
                }

                sai_route_entry_t route_entry;
                route_entry.vr_id = vrf_id;
                route_entry.switch_id = gSwitchId;
                copy(route_entry.destination, ip_prefix);

                if (nhg.getSize() == 1 && nhg.hasIntfNextHop())
                {
                    if (alsv[0] == "unknown")
                    {
                        it = consumer.m_toSync.erase(it);
                    }
                    /* skip direct routes to tun0 */
                    else if (alsv[0] == "tun0")
                    {
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
                        /* The prefix is full mask (/32 or /128) and it is an interface subnet route, so IntfOrch has already
                         * created an IP2ME route for it and we skip programming such route here as it already exists.
                         * However, to keep APPL_DB and APPL_STATE_DB consistent we have to publish it. */
                        publishRouteState(ctx);
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
                /*
                 * Check if the route does not exist or needs to be updated or
                 * if the route is using a temporary next hop group owned by
                 * NhgOrch.
                 */
                else if (m_syncdRoutes.find(vrf_id) == m_syncdRoutes.end() ||
                    m_syncdRoutes.at(vrf_id).find(ip_prefix) == m_syncdRoutes.at(vrf_id).end() ||
                    m_syncdRoutes.at(vrf_id).at(ip_prefix) != RouteNhg(nhg, ctx.nhg_index) ||
                    gRouteBulker.bulk_entry_pending_removal(route_entry) ||
                    ctx.using_temp_nhg)
                {
                    if (addRoute(ctx, nhg))
                        it = consumer.m_toSync.erase(it);
                    else
                        it++;
                }
                else
                {
                    /* Duplicate entry. Publish route state anyway since there could be multiple DEL, SET operations
                     * consolidated by ConsumerStateTable leading to orchagent receiving only the last SET update. */
                    publishRouteState(ctx);
                    it = consumer.m_toSync.erase(it);
                }

                // If already exhaust the nexthop groups, and there are pending removing routes in bulker,
                // flush the bulker and possibly collect some released nexthop groups
                if (m_nextHopGroupCount + NhgOrch::getSyncedNhgCount() >= m_maxNextHopGroupCount &&
                    gRouteBulker.removing_entries_count() > 0)
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
        m_bulkNhgReducedRefCnt.clear();
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

                if (nhg.getSize() == 1 && nhg.hasIntfNextHop())
                {
                    if (addRoutePost(ctx, nhg))
                        it_prev = consumer.m_toSync.erase(it_prev);
                    else
                        it_prev++;
                }
                else if (m_syncdRoutes.find(vrf_id) == m_syncdRoutes.end() ||
                         m_syncdRoutes.at(vrf_id).find(ip_prefix) == m_syncdRoutes.at(vrf_id).end() ||
                         m_syncdRoutes.at(vrf_id).at(ip_prefix) != RouteNhg(nhg, ctx.nhg_index) ||
                         ctx.using_temp_nhg)
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

        /* Remove next hop group if the reference count decreases to zero */
        for (auto& it_nhg : m_bulkNhgReducedRefCnt)
        {
            if (it_nhg.first.is_overlay_nexthop() && it_nhg.second != 0)
            {
                removeOverlayNextHops(it_nhg.second, it_nhg.first);
            }
            else if (it_nhg.first.is_srv6_nexthop())
            {
                if(it_nhg.first.getSize() > 1)
                {
                    if(m_syncdNextHopGroups[it_nhg.first].ref_count == 0)
                    {
                      removeNextHopGroup(it_nhg.first);
                    }
                    else
                    {
                      SWSS_LOG_ERROR("SRV6 ECMP %s REF count is not zero", it_nhg.first.to_string().c_str());
                    }
                }
            }
            else if (m_syncdNextHopGroups[it_nhg.first].ref_count == 0)
            {
                removeNextHopGroup(it_nhg.first);
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

                entry.second.routeTable.emplace(prefix, RouteNhg(nexthops, ""));
            }
            else
            {
                if (route->second.nhg_key != nexthops)
                {
                    route->second.nhg_key = nexthops;
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
                    NextHopUpdate update = { vrf_id, entry.first.second, route->first, route->second.nhg_key };

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
        const NextHopKey& nexthop = *nexthops.getNextHops().begin();
        if (nexthop.isIntfNextHop())
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
        const NextHopKey& nexthop = *nexthops.getNextHops().begin();
        if (nexthop.isIntfNextHop())
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

const NextHopGroupKey RouteOrch::getSyncdRouteNhgKey(sai_object_id_t vrf_id, const IpPrefix& ipPrefix)
{
    NextHopGroupKey nhg;
    auto route_table = m_syncdRoutes.find(vrf_id);
    if (route_table != m_syncdRoutes.end())
    {
        auto route_entry = route_table->second.find(ipPrefix);
        if (route_entry != route_table->second.end())
        {
            nhg = route_entry->second.nhg_key;
        }
    }
    return nhg;
}

bool RouteOrch::createFineGrainedNextHopGroup(sai_object_id_t &next_hop_group_id, vector<sai_attribute_t> &nhg_attrs)
{
    SWSS_LOG_ENTER();

    if (m_nextHopGroupCount + NhgOrch::getSyncedNhgCount() >= m_maxNextHopGroupCount)
    {
        SWSS_LOG_DEBUG("Failed to create new next hop group. \
                Reaching maximum number of next hop groups.");
        return false;
    }

    sai_status_t status = sai_next_hop_group_api->create_next_hop_group(&next_hop_group_id,
                                                      gSwitchId,
                                                      (uint32_t)nhg_attrs.size(),
                                                      nhg_attrs.data());
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create next hop group rv:%d", status);
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_NEXT_HOP_GROUP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    m_nextHopGroupCount++;

    return true;
}

bool RouteOrch::removeFineGrainedNextHopGroup(sai_object_id_t &next_hop_group_id)
{
    SWSS_LOG_ENTER();

    sai_status_t status = sai_next_hop_group_api->remove_next_hop_group(next_hop_group_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove next hop group %" PRIx64 ", rv:%d",
                next_hop_group_id, status);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEXT_HOP_GROUP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    m_nextHopGroupCount--;

    return true;
}

bool RouteOrch::addNextHopGroup(const NextHopGroupKey &nexthops)
{
    SWSS_LOG_ENTER();

    assert(!hasNextHopGroup(nexthops));

    if (m_nextHopGroupCount + NhgOrch::getSyncedNhgCount() >= m_maxNextHopGroupCount)
    {
        SWSS_LOG_DEBUG("Failed to create new next hop group. \
                        Reaching maximum number of next hop groups.");
        return false;
    }

    vector<sai_object_id_t> next_hop_ids;
    set<NextHopKey> next_hop_set = nexthops.getNextHops();
    std::map<sai_object_id_t, NextHopKey> nhopgroup_members_set;
    std::map<sai_object_id_t, set<NextHopKey>> nhopgroup_shared_set;

    /* Assert each IP address exists in m_syncdNextHops table,
     * and add the corresponding next_hop_id to next_hop_ids. */
    for (auto it : next_hop_set)
    {
        sai_object_id_t next_hop_id;
        if (m_neighOrch->hasNextHop(it))
        {
            next_hop_id = m_neighOrch->getNextHopId(it);
        }
        /* See if there is an IP neighbor NH for MPLS NH*/
        else if (it.isMplsNextHop() &&
                 m_neighOrch->hasNextHop(NextHopKey(it.ip_address, it.alias)))
        {
            m_neighOrch->addNextHop(it);
            next_hop_id = m_neighOrch->getNextHopId(it);
        }
        else
        {
            SWSS_LOG_INFO("Failed to get next hop %s in %s",
                    it.to_string().c_str(), nexthops.to_string().c_str());
            return false;
        }
        // skip next hop group member create for neighbor from down port
        if (m_neighOrch->isNextHopFlagSet(it, NHFLAGS_IFDOWN))
        {
            SWSS_LOG_INFO("Interface down for NH %s, skip this NH", it.to_string().c_str());
            continue;
        }

        next_hop_ids.push_back(next_hop_id);
        if (nhopgroup_members_set.find(next_hop_id) == nhopgroup_members_set.end())
        {
            nhopgroup_members_set[next_hop_id] = it;
        }
        else
        {
            nhopgroup_shared_set[next_hop_id].insert(it);
        }
    }
    if (!next_hop_ids.size())
    {
        SWSS_LOG_INFO("Skipping creation of nexthop group as none of nexthop are active");
        return false;
    }
    sai_attribute_t nhg_attr;
    vector<sai_attribute_t> nhg_attrs;

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    nhg_attr.value.s32 = m_switchOrch->checkOrderedEcmpEnable() ? SAI_NEXT_HOP_GROUP_TYPE_DYNAMIC_ORDERED_ECMP : SAI_NEXT_HOP_GROUP_TYPE_ECMP;
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
        task_process_status handle_status = handleSaiCreateStatus(SAI_API_NEXT_HOP_GROUP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    m_nextHopGroupCount++;
    SWSS_LOG_NOTICE("Create next hop group %s", nexthops.to_string().c_str());

    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);

    NextHopGroupEntry next_hop_group_entry;
    next_hop_group_entry.next_hop_group_id = next_hop_group_id;

    size_t npid_count = next_hop_ids.size();
    vector<sai_object_id_t> nhgm_ids(npid_count);
    for (size_t i = 0; i < npid_count; i++)
    {
        auto nhid = next_hop_ids[i];
        auto weight = nhopgroup_members_set[nhid].weight;

        // Create a next hop group member
        vector<sai_attribute_t> nhgm_attrs;

        sai_attribute_t nhgm_attr;
        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
        nhgm_attr.value.oid = next_hop_group_id;
        nhgm_attrs.push_back(nhgm_attr);

        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
        nhgm_attr.value.oid = nhid;
        nhgm_attrs.push_back(nhgm_attr);

        if (weight)
        {
            nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT;
            nhgm_attr.value.s32 = weight;
            nhgm_attrs.push_back(nhgm_attr);
        }

        if (m_switchOrch->checkOrderedEcmpEnable())
        {
            nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_SEQUENCE_ID;
            nhgm_attr.value.u32 = ((uint32_t)i) + 1; // To make non-zero sequence id
            nhgm_attrs.push_back(nhgm_attr);
        }

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
        if (nhopgroup_shared_set.find(nhid) != nhopgroup_shared_set.end())
        {
            auto it = nhopgroup_shared_set[nhid].begin();
            next_hop_group_entry.nhopgroup_members[*it].next_hop_id = nhgm_id;
            next_hop_group_entry.nhopgroup_members[*it].seq_id = (uint32_t)i + 1;
            nhopgroup_shared_set[nhid].erase(it);
            if (nhopgroup_shared_set[nhid].empty())
            {
                nhopgroup_shared_set.erase(nhid);
            }
        }
        else
        {
            next_hop_group_entry.nhopgroup_members[nhopgroup_members_set.find(nhid)->second].next_hop_id = nhgm_id;
            next_hop_group_entry.nhopgroup_members[nhopgroup_members_set.find(nhid)->second].seq_id = ((uint32_t)i) + 1;
        }
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
    bool overlay_nh = nexthops.is_overlay_nexthop();
    bool srv6_nh = nexthops.is_srv6_nexthop();

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
                           nhop->first.to_string().c_str(), nhop->second.next_hop_id);
            nhop = nhgm.erase(nhop);
            continue;
        }

        next_hop_ids.push_back(nhop->second.next_hop_id);
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
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEXT_HOP_GROUP, statuses[i]);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    status = sai_next_hop_group_api->remove_next_hop_group(next_hop_group_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove next hop group %" PRIx64 ", rv:%d", next_hop_group_id, status);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_NEXT_HOP_GROUP, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    m_nextHopGroupCount--;
    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);

    set<NextHopKey> next_hop_set = nexthops.getNextHops();
    for (auto it : next_hop_set)
    {
        m_neighOrch->decreaseNextHopRefCount(it);
        if (overlay_nh && !srv6_nh && !m_neighOrch->getNextHopRefCount(it))
        {
            if(!m_neighOrch->removeTunnelNextHop(it))
            {
                SWSS_LOG_ERROR("Tunnel Nexthop %s delete failed", nexthops.to_string().c_str());
            }
            else
            {
                m_neighOrch->removeOverlayNextHop(it);
                SWSS_LOG_INFO("Tunnel Nexthop %s delete success", nexthops.to_string().c_str());
                SWSS_LOG_INFO("delete remote vtep %s", it.to_string(overlay_nh, srv6_nh).c_str());
                status = deleteRemoteVtep(SAI_NULL_OBJECT_ID, it);
                if (status == false)
                {
                    SWSS_LOG_ERROR("Failed to delete remote vtep %s ecmp", it.to_string(overlay_nh, srv6_nh).c_str());
                }
            }
        }
        /* Remove any MPLS-specific NH that was created */
        else if (it.isMplsNextHop() &&
                 (m_neighOrch->getNextHopRefCount(it) == 0))
        {
            m_neighOrch->removeMplsNextHop(it);
        }
    }

    if (srv6_nh)
    {
        if (!m_srv6Orch->removeSrv6Nexthops(nexthops))
        {
            SWSS_LOG_ERROR("Failed to remove Srv6 Nexthop %s", nexthops.to_string().c_str());
        }
        else
        {
            SWSS_LOG_INFO("Remove ECMP Srv6 nexthops %s", nexthops.to_string().c_str());
        }
    }

    m_syncdNextHopGroups.erase(nexthops);

    return true;
}

void RouteOrch::addNextHopRoute(const NextHopKey& nextHop, const RouteKey& routeKey)
{
    auto it = m_nextHops.find((nextHop));

    if (it != m_nextHops.end())
    {
        if (it->second.find(routeKey) != it->second.end())
        {
            SWSS_LOG_INFO("Route already present in nh table %s",
                          routeKey.prefix.to_string().c_str());
            return;
        }

        it->second.insert(routeKey);
    }
    else
    {
        set<RouteKey> routes;
        routes.insert(routeKey);
        m_nextHops.insert(make_pair(nextHop, routes));
    }
}

void RouteOrch::removeNextHopRoute(const NextHopKey& nextHop, const RouteKey& routeKey)
{
    auto it = m_nextHops.find((nextHop));

    if (it != m_nextHops.end())
    {
        if (it->second.find(routeKey) == it->second.end())
        {
            SWSS_LOG_INFO("Route not present in nh table %s", routeKey.prefix.to_string().c_str());
            return;
        }

        it->second.erase(routeKey);
        if (it->second.empty())
        {
            m_nextHops.erase(nextHop);
        }
    }
    else
    {
        SWSS_LOG_INFO("Nexthop %s not found in nexthop table", nextHop.to_string().c_str());
    }
}

bool RouteOrch::updateNextHopRoutes(const NextHopKey& nextHop, uint32_t& numRoutes)
{
    numRoutes = 0;
    auto it = m_nextHops.find((nextHop));

    if (it == m_nextHops.end())
    {
        SWSS_LOG_INFO("No routes found for NH %s", nextHop.ip_address.to_string().c_str());
        return true;
    }

    sai_route_entry_t route_entry;
    sai_attribute_t route_attr;
    sai_object_id_t next_hop_id;

    auto rt = it->second.begin();
    while(rt != it->second.end())
    {
        /* Check if route points to nexthop group and skip */
        NextHopGroupKey nhg_key = gRouteOrch->getSyncdRouteNhgKey(gVirtualRouterId, (*rt).prefix);
        if (nhg_key.getSize() > 1)
        {
            /* multiple mux nexthop case:
             * skip for now, muxOrch::updateRoute() will handle route
             */
            SWSS_LOG_INFO("Route %s is mux multi nexthop route, skipping.",
                        (*rt).prefix.to_string().c_str());

            ++rt;
            continue;
        }

        SWSS_LOG_INFO("Updating route %s", (*rt).prefix.to_string().c_str());
        next_hop_id = m_neighOrch->getNextHopId(nextHop);

        route_entry.vr_id = (*rt).vrf_id;
        route_entry.switch_id = gSwitchId;
        copy(route_entry.destination, (*rt).prefix);

        route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        route_attr.value.oid = next_hop_id;

        sai_status_t status = sai_route_api->set_route_entry_attribute(&route_entry, &route_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to update route %s, rv:%d", (*rt).prefix.to_string().c_str(), status);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_ROUTE, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        ++numRoutes;
        ++rt;
    }

    return true;
}

/**
 * @brief returns a route prefix associated with nexthopkey
 * @param routeKeys empty set of routekeys to populate
 * @param nexthopKey nexthop key to lookup
 * @return true if found, false if not found.
 */
bool RouteOrch::getRoutesForNexthop(std::set<RouteKey>& routeKeys, const NextHopKey& nexthopKey)
{
    auto it = m_nextHops.find(nexthopKey);

    if (it != m_nextHops.end())
    {
        routeKeys = it->second;
    }

    return it != m_nextHops.end();
}

void RouteOrch::addTempRoute(RouteBulkContext& ctx, const NextHopGroupKey &nextHops)
{
    SWSS_LOG_ENTER();

    IpPrefix& ipPrefix = ctx.ip_prefix;

    auto next_hop_set = nextHops.getNextHops();

    /* Remove next hops that are not in m_syncdNextHops */
    for (auto it = next_hop_set.begin(); it != next_hop_set.end();)
    {
        /*
         * Check if the IP next hop exists in NeighOrch.  The next hop may be
         * a labeled one, which are created by RouteOrch or NhgOrch if the IP
         * next hop exists.
         */
        if (!m_neighOrch->isNeighborResolved(*it))
        {
            SWSS_LOG_INFO("Failed to get next hop %s for %s",
                   (*it).to_string().c_str(), ipPrefix.to_string().c_str());
            it = next_hop_set.erase(it);
            continue;
        }
        if(m_neighOrch->isNextHopFlagSet(*it, NHFLAGS_IFDOWN))
        {
            SWSS_LOG_INFO("Interface down for NH %s, skip this NH", (*it).to_string().c_str());
            it = next_hop_set.erase(it);
            continue;
        }
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

    /* next_hop_id indicates the next hop id or next hop group id of this route */
    sai_object_id_t next_hop_id = SAI_NULL_OBJECT_ID;
    bool overlay_nh = false;
    bool status = false;
    bool curNhgIsFineGrained = false;
    bool isFineGrainedNextHopIdChanged = false;
    bool blackhole = false;
    bool srv6_nh = false;

    if (m_syncdRoutes.find(vrf_id) == m_syncdRoutes.end())
    {
        m_syncdRoutes.emplace(vrf_id, RouteTable());
        m_vrfOrch->increaseVrfRefCount(vrf_id);
    }

    if (nextHops.is_overlay_nexthop())
    {
        overlay_nh = true;
    }

    if (nextHops.is_srv6_nexthop())
    {
        srv6_nh = true;
    }

    auto it_route = m_syncdRoutes.at(vrf_id).find(ipPrefix);

    if (m_fgNhgOrch->isRouteFineGrained(vrf_id, ipPrefix, nextHops))
    {
        /* The route is pointing to a Fine Grained nexthop group */
        curNhgIsFineGrained = true;
        /* We get 3 return values from setFgNhg:
         * 1. success/failure: on addition/modification of nexthop group/members
         * 2. next_hop_id: passed as a param to fn, used for sai route creation
         * 3. isFineGrainedNextHopIdChanged: passed as a param to fn, used to determine transitions
         * between regular and FG ECMP, this is an optimization to prevent multiple lookups */
        if (!m_fgNhgOrch->setFgNhg(vrf_id, ipPrefix, nextHops, next_hop_id, isFineGrainedNextHopIdChanged))
        {
            return false;
        }
    }
    /* NhgOrch owns the NHG */
    else if (!ctx.nhg_index.empty())
    {
        try
        {
            const NhgBase& nhg = getNhg(ctx.nhg_index);
            next_hop_id = nhg.getId();
        }
        catch(const std::out_of_range& e)
        {
            SWSS_LOG_INFO("Next hop group key %s does not exist", ctx.nhg_index.c_str());
            return false;
        }
    }
    /* RouteOrch owns the NHG */
    else if (nextHops.getSize() == 0)
    {
        /* The route is pointing to a blackhole */
        blackhole = true;
    }
    else if (nextHops.getSize() == 1)
    {
        /* The route is pointing to a next hop */
        const NextHopKey& nexthop = *nextHops.getNextHops().begin();
        if (nexthop.isIntfNextHop())
        {
            if(gPortsOrch->isInbandPort(nexthop.alias))
            {
                //This routes is the static route added for the remote system neighbors
                //We do not need this route in the ASIC since the static neighbor creation
                //in ASIC adds the same full mask route (host route) in ASIC automatically
                //So skip.
                return true;
            }

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
            /* For non-existent MPLS NH, check if IP neighbor NH exists */
            else if (nexthop.isMplsNextHop() &&
                     m_neighOrch->isNeighborResolved(nexthop))
            {
                /* since IP neighbor NH exists, neighbor is resolved, add MPLS NH */
                m_neighOrch->addNextHop(nexthop);
                next_hop_id = m_neighOrch->getNextHopId(nexthop);
            }
            /* IP neighbor is not yet resolved */
            else
            {
                if(overlay_nh && !srv6_nh)
                {
                    SWSS_LOG_INFO("create remote vtep %s", nexthop.to_string(overlay_nh, srv6_nh).c_str());
                    status = createRemoteVtep(vrf_id, nexthop);
                    if (status == false)
                    {
                        SWSS_LOG_ERROR("Failed to create remote vtep %s", nexthop.to_string(overlay_nh, srv6_nh).c_str());
                        return false;
                    }
                    next_hop_id = m_neighOrch->addTunnelNextHop(nexthop);
                    if (next_hop_id == SAI_NULL_OBJECT_ID)
                    {
                        SWSS_LOG_ERROR("Failed to create Tunnel Nexthop %s", nexthop.to_string(overlay_nh, srv6_nh).c_str());
                        return false;
                    }
                }
                else if (srv6_nh)
                {
                    SWSS_LOG_INFO("Single NH: create srv6 nexthop %s", nextHops.to_string().c_str());
                    if (!m_srv6Orch->srv6Nexthops(nextHops, next_hop_id))
                    {
                        SWSS_LOG_ERROR("Failed to create SRV6 nexthop %s", nextHops.to_string().c_str());
                        return false;
                    }
                }
                else
                {
                    SWSS_LOG_INFO("Failed to get next hop %s for %s, resolving neighbor",
                            nextHops.to_string().c_str(), ipPrefix.to_string().c_str());
                    m_neighOrch->resolveNeighbor(nexthop);
                    return false;
                }
            }
        }
    }
    /* The route is pointing to a next hop group */
    else
    {
        /* Check if there is already an existing next hop group */
        if (!hasNextHopGroup(nextHops))
        {
            if(srv6_nh)
            {
                sai_object_id_t temp_nh_id;
                SWSS_LOG_INFO("ECMP SRV6 NH: create srv6 nexthops %s", nextHops.to_string().c_str());
                if(!m_srv6Orch->srv6Nexthops(nextHops, temp_nh_id))
                {
                    SWSS_LOG_ERROR("Failed to create SRV6 nexthops for %s", nextHops.to_string().c_str());
                    return false;
                }
            }
            /* Try to create a new next hop group */
            if (!addNextHopGroup(nextHops))
            {
                for(auto it = nextHops.getNextHops().begin(); it != nextHops.getNextHops().end(); ++it)
                {
                    const NextHopKey& nextHop = *it;
                    if(!m_neighOrch->hasNextHop(nextHop))
                    {
                        if(overlay_nh)
                        {
                            SWSS_LOG_INFO("create remote vtep %s ecmp", nextHop.to_string(overlay_nh, srv6_nh).c_str());
                            status = createRemoteVtep(vrf_id, nextHop);
                            if (status == false)
                            {
                                SWSS_LOG_ERROR("Failed to create remote vtep %s ecmp", nextHop.to_string(overlay_nh, srv6_nh).c_str());
                                return false;
                            }
                            next_hop_id = m_neighOrch->addTunnelNextHop(nextHop);
                            if (next_hop_id == SAI_NULL_OBJECT_ID)
                            {
                                SWSS_LOG_ERROR("Failed to create Tunnel Nexthop %s", nextHop.to_string(overlay_nh, srv6_nh).c_str());
                                return false;
                            }
                        }
                        else
                        {
                            SWSS_LOG_INFO("Failed to get next hop %s in %s, resolving neighbor",
                                    nextHop.to_string().c_str(), nextHops.to_string().c_str());
                            m_neighOrch->resolveNeighbor(nextHop);
                        }
                    }
                }

                /* Failed to create the next hop group and check if a temporary route is needed */

                /* If the current next hop is part of the next hop group to sync,
                 * then return false and no need to add another temporary route. */
                if (it_route != m_syncdRoutes.at(vrf_id).end() && it_route->second.nhg_key.getSize() == 1)
                {
                    const NextHopKey& nexthop = *it_route->second.nhg_key.getNextHops().begin();
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
     *
     * In case the entry is already pending removal in the bulk, it would be removed
     * from m_syncdRoutes during the bulk call. Therefore, such entries need to be
     * re-created rather than set attribute.
     */
    if (it_route == m_syncdRoutes.at(vrf_id).end() || gRouteBulker.bulk_entry_pending_removal(route_entry))
    {
        if (blackhole)
        {
            route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
            route_attr.value.s32 = SAI_PACKET_ACTION_DROP;
        }
        else
        {
            route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
            route_attr.value.oid = next_hop_id;
        }

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
        /* Set the packet action to forward when there was no next hop (dropped) and not pointing to blackhole*/
        if (it_route->second.nhg_key.getSize() == 0 && !blackhole)
        {
            route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
            route_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;

            object_statuses.emplace_back();
            gRouteBulker.set_entry_attribute(&object_statuses.back(), &route_entry, &route_attr);
        }

        if (curNhgIsFineGrained && !isFineGrainedNextHopIdChanged)
        {
            /* Don't change route entry if the route is previously fine grained and new nhg is also fine grained.
             * We already modifed sai nhg objs as part of setFgNhg to account for nhg change. */
            object_statuses.emplace_back(SAI_STATUS_SUCCESS);
        }
        else
        {
            if (!blackhole && vrf_id == gVirtualRouterId && ipPrefix.isDefaultRoute())
            {
                // Always set packet action for default route to avoid conflict settings
                // in case a SET follows a DEL on the default route in the same bulk.
                // - On DEL default route, the packet action will be set to DROP
                // - On SET default route, as the default route has NOT been removed from m_syncdRoute
                //   it calls SAI set_route_attributes instead of crate_route
                //   However, packet action is called only when a route entry is created
                //   This leads to conflict settings:
                //   - packet action: DROP
                //   - next hop: a valid next hop id
                // To avoid this, we always set packet action for default route.
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

        if (blackhole)
        {
            route_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
            route_attr.value.s32 = SAI_PACKET_ACTION_DROP;

            object_statuses.emplace_back();
            gRouteBulker.set_entry_attribute(&object_statuses.back(), &route_entry, &route_attr);
        }
    }
    return false;
}

bool RouteOrch::addRoutePost(const RouteBulkContext& ctx, const NextHopGroupKey &nextHops)
{
    SWSS_LOG_ENTER();

    const sai_object_id_t& vrf_id = ctx.vrf_id;
    const IpPrefix& ipPrefix = ctx.ip_prefix;
    bool isFineGrained = false;
    bool blackhole = false;

    const auto& object_statuses = ctx.object_statuses;

    if (object_statuses.empty())
    {
        // Something went wrong before router bulker, will retry
        return false;
    }

    if (m_fgNhgOrch->isRouteFineGrained(vrf_id, ipPrefix, nextHops))
    {
        /* Route is pointing to Fine Grained ECMP nexthop group */
        isFineGrained = true;
    }
    /* NhgOrch owns the NHG. */
    else if (!ctx.nhg_index.empty())
    {
        if (!gNhgOrch->hasNhg(ctx.nhg_index) && !gCbfNhgOrch->hasNhg(ctx.nhg_index))
        {
            SWSS_LOG_INFO("Failed to get next hop group with index %s", ctx.nhg_index.c_str());
            return false;
        }
    }
    /* RouteOrch owns the NHG */
    else if (nextHops.getSize() == 0)
    {
        /* The route is pointing to a blackhole */
        blackhole = true;
    }
    else if (nextHops.getSize() == 1)
    {
        /* The route is pointing to a next hop */
        const NextHopKey& nexthop = *nextHops.getNextHops().begin();
        if (nexthop.isIntfNextHop())
        {
            auto next_hop_id = m_intfsOrch->getRouterIntfsId(nexthop.alias);
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
    MuxOrch* mux_orch = gDirectory.get<MuxOrch*>();
    if (isFineGrained)
    {
        if (it_route == m_syncdRoutes.at(vrf_id).end())
        {
            /* First time route addition pointing to FG nhg */
            if (*it_status++ != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create route %s with next hop(s) %s",
                        ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
                /* Clean up the newly created next hop group entry */
                m_fgNhgOrch->removeFgNhg(vrf_id, ipPrefix);
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
            SWSS_LOG_INFO("FG Post create route %s with next hop(s) %s",
                    ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
        }
        else
        {
            /* Route already exists */
            auto nh_entry = m_syncdNextHopGroups.find(it_route->second.nhg_key);
            if (nh_entry != m_syncdNextHopGroups.end())
            {
                /* Case where route was pointing to non-fine grained nhs in the past,
                 * and transitioned to Fine Grained ECMP */
                decreaseNextHopRefCount(it_route->second.nhg_key);
                if (it_route->second.nhg_key.getSize() > 1
                    && m_syncdNextHopGroups[it_route->second.nhg_key].ref_count == 0)
                {
                    m_bulkNhgReducedRefCnt.emplace(it_route->second.nhg_key, 0);
                }
            }
            SWSS_LOG_INFO("FG Post set route %s with next hop(s) %s",
                    ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
        }
    }
    else if (it_route == m_syncdRoutes.at(vrf_id).end())
    {
        sai_status_t status = *it_status++;
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create route %s with next hop(s) %s",
                    ipPrefix.to_string().c_str(), nextHops.to_string().c_str());

            /* Check that the next hop group is not owned by NhgOrch. */
            if (ctx.nhg_index.empty() && nextHops.getSize() > 1)
            {
                /* Clean up the newly created next hop group entry */
                removeNextHopGroup(nextHops);
            }
            task_process_status handle_status = handleSaiCreateStatus(SAI_API_ROUTE, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        if (ipPrefix.isV4())
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV4_ROUTE);
        }
        else
        {
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_IPV6_ROUTE);
        }

        /* Increase the ref_count for the next hop group. */
        if (ctx.nhg_index.empty())
        {
            increaseNextHopRefCount(nextHops);
        }
        else
        {
            incNhgRefCount(ctx.nhg_index);
        }

        SWSS_LOG_INFO("Post create route %s with next hop(s) %s",
                ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
    }
    else
    {
        sai_status_t status;

        /* Set the packet action to forward when there was no next hop (dropped) and not pointing to blackhole */
        if (it_route->second.nhg_key.getSize() == 0 && !blackhole)
        {
            status = *it_status++;
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set route %s with packet action forward, %d",
                               ipPrefix.to_string().c_str(), status);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_ROUTE, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
        }

        status = *it_status++;
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set route %s with next hop(s) %s",
                    ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
            task_process_status handle_status = handleSaiSetStatus(SAI_API_ROUTE, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        if (m_fgNhgOrch->syncdContainsFgNhg(vrf_id, ipPrefix))
        {
            /* Remove FG nhg since prefix now points to standard nhg/nhs */
            m_fgNhgOrch->removeFgNhg(vrf_id, ipPrefix);
        }
        /* Decrease the ref count for the previous next hop group. */
        else if (it_route->second.nhg_index.empty())
        {
            decreaseNextHopRefCount(it_route->second.nhg_key);
            auto ol_nextHops = it_route->second.nhg_key;
            if (ol_nextHops.getSize() > 1)
            {
                if (m_syncdNextHopGroups[ol_nextHops].ref_count == 0)
                {
                    SWSS_LOG_NOTICE("Update Nexthop Group %s", ol_nextHops.to_string().c_str());
                    m_bulkNhgReducedRefCnt.emplace(ol_nextHops, 0);
                }
                if (mux_orch->isMuxNexthops(ol_nextHops))
                {
                    SWSS_LOG_NOTICE("Remove mux Nexthop %s", ol_nextHops.to_string().c_str());
                    RouteKey routekey = { vrf_id, ipPrefix };
                    auto nexthop_list = ol_nextHops.getNextHops();
                    for (auto nh = nexthop_list.begin(); nh != nexthop_list.end(); nh++)
                    {
                        if (!nh->ip_address.isZero())
                        {
                            removeNextHopRoute(*nh, routekey);
                        }
                    }
                }
            }
            else if (ol_nextHops.is_overlay_nexthop())
            {
                const NextHopKey& nexthop = *it_route->second.nhg_key.getNextHops().begin();
                if (m_neighOrch->getNextHopRefCount(nexthop) == 0)
                {
                    SWSS_LOG_NOTICE("Update overlay Nexthop %s", ol_nextHops.to_string().c_str());
                    m_bulkNhgReducedRefCnt.emplace(ol_nextHops, vrf_id);
                }
            }
            else if (ol_nextHops.is_srv6_nexthop())
            {
                m_srv6Orch->removeSrv6Nexthops(ol_nextHops);
            }
            else if (ol_nextHops.getSize() == 1)
            {
                RouteKey r_key = { vrf_id, ipPrefix };
                auto nexthop = NextHopKey(ol_nextHops.to_string());
                removeNextHopRoute(nexthop, r_key);
            }
        }
        /* The next hop group is owned by (Cbf)NhgOrch. */
        else
        {
            decNhgRefCount(it_route->second.nhg_index);
        }

        if (blackhole)
        {
            /* Set the packet action to drop for blackhole routes */
            status = *it_status++;
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set blackhole route %s with packet action drop, %d",
                                ipPrefix.to_string().c_str(), status);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_ROUTE, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
        }

        if (ctx.nhg_index.empty())
        {
            /* Increase the ref_count for the next hop (group) entry */
            increaseNextHopRefCount(nextHops);
        }
        else
        {
            incNhgRefCount(ctx.nhg_index);
        }

        SWSS_LOG_INFO("Post set route %s with next hop(s) %s",
                ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
    }

    if (ctx.nhg_index.empty() && nextHops.getSize() == 1 && !nextHops.is_overlay_nexthop() && !nextHops.is_srv6_nexthop())
    {
        RouteKey r_key = { vrf_id, ipPrefix };
        auto nexthop = NextHopKey(nextHops.to_string());
        if (!nexthop.ip_address.isZero())
        {
            addNextHopRoute(nexthop, r_key);
        }
    }
    else if (mux_orch->isMuxNexthops(nextHops))
    {
        RouteKey routekey = { vrf_id, ipPrefix };
        auto nexthop_list = nextHops.getNextHops();
        for (auto nh = nexthop_list.begin(); nh != nexthop_list.end(); nh++)
        {
            if (!nh->ip_address.isZero())
            {
                addNextHopRoute(*nh, routekey);
            }
        }
    }

    if (ipPrefix.isDefaultRoute())
    {
        updateDefRouteState(ipPrefix.to_string(), true);
    }

    if (it_route == m_syncdRoutes.at(vrf_id).end())
    {
        gFlowCounterRouteOrch->handleRouteAdd(vrf_id, ipPrefix);
    }

    m_syncdRoutes[vrf_id][ipPrefix] = RouteNhg(nextHops, ctx.nhg_index);

    /* add subnet decap term for VIP route */
    const SubnetDecapConfig &config = gTunneldecapOrch->getSubnetDecapConfig();
    if (config.enable && isVipRoute(ipPrefix, nextHops))
    {
        createVipRouteSubnetDecapTerm(ipPrefix);
    }

    // update routes to reflect mux state
    if (mux_orch->isMuxNexthops(nextHops))
    {
        mux_orch->updateRoute(ipPrefix, true);
    }

    notifyNextHopChangeObservers(vrf_id, ipPrefix, nextHops, true);

    /* Publish and update APPL STATE DB route entry programming status */
    publishRouteState(ctx);

    /*
     * If the route uses a temporary synced NHG owned by NhgOrch, return false
     * in order to keep trying to update the route in case the NHG is updated,
     * which will update the SAI ID of the group as well.
     */
    return !ctx.using_temp_nhg;
}

bool RouteOrch::removeRoute(RouteBulkContext& ctx)
{
    SWSS_LOG_ENTER();

    sai_object_id_t& vrf_id = ctx.vrf_id;
    IpPrefix& ipPrefix = ctx.ip_prefix;

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
       if (it_route_table->second.size() == 0)
       {
            m_syncdRoutes.erase(vrf_id);
            m_vrfOrch->decreaseVrfRefCount(vrf_id);
       }
       SWSS_LOG_INFO("Failed to find route entry, vrf_id 0x%" PRIx64 ", prefix %s\n", vrf_id,
                     ipPrefix.to_string().c_str());
 
        return true;
    }

    auto& object_statuses = ctx.object_statuses;

    // set to blackhole for default route
    if (ipPrefix.isDefaultRoute() && vrf_id == gVirtualRouterId)
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
    if (ipPrefix.isDefaultRoute() && vrf_id == gVirtualRouterId)
    {
        sai_status_t status = *it_status++;
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set route %s packet action to drop, rv:%d",
                    ipPrefix.to_string().c_str(), status);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_ROUTE, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        SWSS_LOG_INFO("Set route %s packet action to drop", ipPrefix.to_string().c_str());

        status = *it_status++;
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set route %s next hop ID to NULL, rv:%d",
                    ipPrefix.to_string().c_str(), status);
            task_process_status handle_status = handleSaiSetStatus(SAI_API_ROUTE, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        updateDefRouteState(ipPrefix.to_string());

        SWSS_LOG_INFO("Set route %s next hop ID to NULL", ipPrefix.to_string().c_str());
    }
    else
    {
        sai_status_t status = *it_status++;
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove route prefix:%s\n", ipPrefix.to_string().c_str());
            task_process_status handle_status = handleSaiRemoveStatus(SAI_API_ROUTE, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
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

    if (m_fgNhgOrch->syncdContainsFgNhg(vrf_id, ipPrefix))
    {
        /* Delete Fine Grained nhg if the revmoved route pointed to it */
        m_fgNhgOrch->removeFgNhg(vrf_id, ipPrefix);
    }
    /* Check if the next hop group is not owned by NhgOrch. */
    else if (!it_route->second.nhg_index.empty())
    {
        decNhgRefCount(it_route->second.nhg_index);
    }
    /* The NHG is owned by RouteOrch */
    else
    {
        /*
         * Decrease the reference count only when the route is pointing to a next hop.
         */
        decreaseNextHopRefCount(it_route->second.nhg_key);

        auto ol_nextHops = it_route->second.nhg_key;
        MuxOrch* mux_orch = gDirectory.get<MuxOrch*>();
        if (it_route->second.nhg_key.getSize() > 1)
        {
            if (m_syncdNextHopGroups[it_route->second.nhg_key].ref_count == 0)
            {
                SWSS_LOG_NOTICE("Remove Nexthop Group %s", ol_nextHops.to_string().c_str());
                m_bulkNhgReducedRefCnt.emplace(it_route->second.nhg_key, 0);
            }
            if (mux_orch->isMuxNexthops(ol_nextHops))
            {
                SWSS_LOG_NOTICE("Remove mux Nexthop %s", ol_nextHops.to_string().c_str());
                RouteKey routekey = { vrf_id, ipPrefix };
                auto nexthop_list = ol_nextHops.getNextHops();
                for (auto nh = nexthop_list.begin(); nh != nexthop_list.end(); nh++)
                {
                    if (!nh->ip_address.isZero())
                    {
                        SWSS_LOG_NOTICE("removeNextHopRoute");
                        removeNextHopRoute(*nh, routekey);
                    }
                }
                mux_orch->updateRoute(ipPrefix, false);
            }
        }
        else if (ol_nextHops.is_overlay_nexthop())
        {
            const NextHopKey& nexthop = *it_route->second.nhg_key.getNextHops().begin();
            if (m_neighOrch->getNextHopRefCount(nexthop) == 0)
            {
                SWSS_LOG_NOTICE("Remove overlay Nexthop %s", ol_nextHops.to_string().c_str());
                m_bulkNhgReducedRefCnt.emplace(ol_nextHops, vrf_id);
            }
        }
        /*
         * Additionally check if the NH has label and its ref count == 0, then
         * remove the label next hop.
         */
        else if (it_route->second.nhg_key.getSize() == 1)
        {
            const NextHopKey& nexthop = *it_route->second.nhg_key.getNextHops().begin();
            if (nexthop.isMplsNextHop() &&
                (m_neighOrch->getNextHopRefCount(nexthop) == 0))
            {
                m_neighOrch->removeMplsNextHop(nexthop);
            }
            else if (nexthop.isSrv6NextHop() &&
                    (m_neighOrch->getNextHopRefCount(nexthop) == 0))
            {
                m_srv6Orch->removeSrv6Nexthops(it_route->second.nhg_key);
            }

            RouteKey r_key = { vrf_id, ipPrefix };
            removeNextHopRoute(nexthop, r_key);
        }
    }

    SWSS_LOG_INFO("Remove route %s with next hop(s) %s",
            ipPrefix.to_string().c_str(), it_route->second.nhg_key.to_string().c_str());

    /* Publish removal status, removes route entry from APPL STATE DB */
    publishRouteState(ctx);

    /* Remove the VIP route subnet decap term */
    removeVipRouteSubnetDecapTerm(ipPrefix);


    if (ipPrefix.isDefaultRoute() && vrf_id == gVirtualRouterId)
    {
        it_route_table->second[ipPrefix] = RouteNhg();

        /* Notify about default route next hop change */
        notifyNextHopChangeObservers(vrf_id, ipPrefix, it_route_table->second[ipPrefix].nhg_key, true);
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

        gFlowCounterRouteOrch->handleRouteRemove(vrf_id, ipPrefix);
    }

    return true;
}

bool RouteOrch::createRemoteVtep(sai_object_id_t vrf_id, const NextHopKey &nextHop)
{
    SWSS_LOG_ENTER();
    EvpnNvoOrch* evpn_orch = gDirectory.get<EvpnNvoOrch*>();
    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    bool status = false;
    int ip_refcnt = 0;

    status = tunnel_orch->addTunnelUser(nextHop.ip_address.to_string(), nextHop.vni, 0, TUNNEL_USER_IP, vrf_id);

    auto vtep_ptr = evpn_orch->getEVPNVtep();
    if (vtep_ptr)
    {
        ip_refcnt = vtep_ptr->getRemoteEndPointIPRefCnt(nextHop.ip_address.to_string());
    }
    SWSS_LOG_INFO("Routeorch Add Remote VTEP %s, VNI %d, VR_ID %" PRIx64 ", IP ref_cnt %d",
            nextHop.ip_address.to_string().c_str(), nextHop.vni, vrf_id, ip_refcnt);
    return status;
}

bool RouteOrch::deleteRemoteVtep(sai_object_id_t vrf_id, const NextHopKey &nextHop)
{
    SWSS_LOG_ENTER();
    EvpnNvoOrch* evpn_orch = gDirectory.get<EvpnNvoOrch*>();
    VxlanTunnelOrch* tunnel_orch = gDirectory.get<VxlanTunnelOrch*>();
    bool status = false;
    int ip_refcnt = 0;

    status = tunnel_orch->delTunnelUser(nextHop.ip_address.to_string(), nextHop.vni, 0, TUNNEL_USER_IP, vrf_id);

    auto vtep_ptr = evpn_orch->getEVPNVtep();
    if (vtep_ptr)
    {
        ip_refcnt = vtep_ptr->getRemoteEndPointIPRefCnt(nextHop.ip_address.to_string());
    }

    SWSS_LOG_INFO("Routeorch Del Remote VTEP %s, VNI %d, VR_ID %" PRIx64 ", IP ref_cnt %d",
            nextHop.ip_address.to_string().c_str(), nextHop.vni, vrf_id, ip_refcnt);
    return status;
}

bool RouteOrch::removeOverlayNextHops(sai_object_id_t vrf_id, const NextHopGroupKey &ol_nextHops)
{
    SWSS_LOG_ENTER();
    bool status = false;

    SWSS_LOG_NOTICE("Remove overlay Nexthop %s", ol_nextHops.to_string().c_str());
    for (auto &tunnel_nh : ol_nextHops.getNextHops())
    {
        if (!m_neighOrch->getNextHopRefCount(tunnel_nh))
        {
            if(!m_neighOrch->removeTunnelNextHop(tunnel_nh))
            {
                SWSS_LOG_ERROR("Tunnel Nexthop %s delete failed", ol_nextHops.to_string().c_str());
            }
            else
            {
                m_neighOrch->removeOverlayNextHop(tunnel_nh);
                SWSS_LOG_INFO("Tunnel Nexthop %s delete success", ol_nextHops.to_string().c_str());
                SWSS_LOG_INFO("delete remote vtep %s", tunnel_nh.to_string(true, false).c_str());
                status = deleteRemoteVtep(vrf_id, tunnel_nh);
                if (status == false)
                {
                    SWSS_LOG_ERROR("Failed to delete remote vtep %s ecmp", tunnel_nh.to_string(true, false).c_str());
                    return false;
                }
            }
        }
    }

    return true;
}

void RouteOrch::increaseNextHopGroupCount()
{
    m_nextHopGroupCount ++;
}

void RouteOrch::decreaseNextHopGroupCount()
{
    m_nextHopGroupCount --;
}

bool RouteOrch::checkNextHopGroupCount()
{
    return m_nextHopGroupCount < m_maxNextHopGroupCount;
}

const NhgBase &RouteOrch::getNhg(const std::string &nhg_index)
{
    SWSS_LOG_ENTER();

    try
    {
        return gNhgOrch->getNhg(nhg_index);
    }
    catch (const std::out_of_range& e)
    {
        return gCbfNhgOrch->getNhg(nhg_index);
    }
}

void RouteOrch::incNhgRefCount(const std::string &nhg_index)
{
    SWSS_LOG_ENTER();

    if (gNhgOrch->hasNhg(nhg_index))
    {
        gNhgOrch->incNhgRefCount(nhg_index);
    }
    else
    {
        gCbfNhgOrch->incNhgRefCount(nhg_index);
    }
}

void RouteOrch::decNhgRefCount(const std::string &nhg_index)
{
    SWSS_LOG_ENTER();

    if (gNhgOrch->hasNhg(nhg_index))
    {
        gNhgOrch->decNhgRefCount(nhg_index);
    }
    else
    {
        gCbfNhgOrch->decNhgRefCount(nhg_index);
    }
}

void RouteOrch::publishRouteState(const RouteBulkContext& ctx, const ReturnCode& status)
{
    SWSS_LOG_ENTER();

    std::vector<FieldValueTuple> fvs;

    /* Leave the fvs empty if the operation type is "DEL".
     * An empty fvs makes ResponsePublisher::publish() remove the state entry from APPL_STATE_DB
     */
    if (ctx.is_set)
    {
        fvs.emplace_back("protocol", ctx.protocol);
    }

    const bool replace = false;

    m_publisher.publish(APP_ROUTE_TABLE_NAME, ctx.key, fvs, status, replace);
}

inline bool RouteOrch::isVipRoute(const IpPrefix &ipPrefix, const NextHopGroupKey &nextHops)
{
    bool res = true;
    /* Ensure all next hops are vlan devices */
    for (const auto &nextHop : nextHops.getNextHops())
    {
        res &= (!nextHop.alias.compare(0, strlen(VLAN_PREFIX), VLAN_PREFIX));
    }
    /* Ensure the prefix is non-local */
    if (nextHops.getSize() == 1)
    {
        res &= (!m_intfsOrch->isPrefixSubnet(ipPrefix, nextHops.getNextHops().begin()->alias));
    }
    return res;
}

inline void RouteOrch::createVipRouteSubnetDecapTerm(const IpPrefix &ipPrefix)
{
    const SubnetDecapConfig &config = gTunneldecapOrch->getSubnetDecapConfig();
    if (!config.enable || m_SubnetDecapTermsCreated.find(ipPrefix) != m_SubnetDecapTermsCreated.end())
    {
        return;
    }
    SWSS_LOG_NOTICE("Add subnet decap term for %s", ipPrefix.to_string().c_str());
    static const vector<FieldValueTuple> data = {
        {"term_type", "MP2MP"},
        {"subnet_type", "vip"}
    };
    string tunnel_name = ipPrefix.isV4() ? config.tunnel : config.tunnel_v6;
    string key = tunnel_name + ":" + ipPrefix.to_string();
    m_appTunnelDecapTermProducer.set(key, data);
    m_SubnetDecapTermsCreated.insert(ipPrefix);
}

inline void RouteOrch::removeVipRouteSubnetDecapTerm(const IpPrefix &ipPrefix)
{
    auto it = m_SubnetDecapTermsCreated.find(ipPrefix);
    if (it == m_SubnetDecapTermsCreated.end())
    {
        return;
    }
    const SubnetDecapConfig &config = gTunneldecapOrch->getSubnetDecapConfig();
    SWSS_LOG_NOTICE("Remove subnet decap term for %s", ipPrefix.to_string().c_str());
    string tunnel_name = ipPrefix.isV4() ? config.tunnel : config.tunnel_v6;
    string key = tunnel_name + ":" + ipPrefix.to_string();
    m_appTunnelDecapTermProducer.del(key);
    m_SubnetDecapTermsCreated.erase(it);
}
