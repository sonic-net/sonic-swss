#include <assert.h>
#include <inttypes.h>
#include <algorithm>
#include "routeorch.h"
#include "logger.h"
#include "swssnet.h"
#include "converter.h"
#include "crmorch.h"
#include "nhgorch.h"
#include "directory.h"
#include "cbf/cbfnhgorch.h"

extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gSwitchId;

extern CrmOrch *gCrmOrch;
extern NhgOrch *gNhgOrch;
extern CbfNhgOrch *gCbfNhgOrch;

void RouteOrch::doLabelTask(ConsumerBase& consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        // Route bulk results will be stored in a map
        std::map<
                std::pair<
                        std::string,            // Key
                        std::string             // Op
                >,
                LabelRouteBulkContext
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
                    SWSS_LOG_NOTICE("Start resync label routes\n");
                    for (auto j : m_syncdLabelRoutes)
                    {
                        string vrf;

                        if (j.first != gVirtualRouterId)
                        {
                            vrf = m_vrfOrch->getVRFname(j.first) + ":";
                        }

                        for (auto i : j.second)
                        {
                            vector<FieldValueTuple> v;
                            key = vrf + to_string(i.first);
                            auto x = KeyOpFieldsValuesTuple(key, DEL_COMMAND, v);
                            consumer.addToSync(x);
                        }
                    }
                    m_resync = true;
                }
                else
                {
                    SWSS_LOG_NOTICE("Complete resync label routes\n");
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
            Label& label = ctx.label;

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
                label = to_uint<uint32_t>(key.substr(found+1));
            }
            else
            {
                vrf_id = gVirtualRouterId;
                label = to_uint<uint32_t>(key);
            }

            if (op == SET_COMMAND)
            {
                string ips;
                string aliases;
                string mpls_nhs;
                uint8_t& pop_count = ctx.pop_count;
                string weights;
                bool& excp_intfs_flag = ctx.excp_intfs_flag;
                bool blackhole = false;
                string nhg_index;

                for (auto i : kfvFieldsValues(t))
                {
                    if (fvField(i) == "nexthop")
                        ips = fvValue(i);

                    if (fvField(i) == "ifname")
                        aliases = fvValue(i);

                    if (fvField(i) == "mpls_nh")
                        mpls_nhs = fvValue(i);

                    if (fvField(i) == "mpls_pop")
                        pop_count = to_uint<uint8_t>(fvValue(i));

                    if (fvField(i) == "blackhole")
                        blackhole = fvValue(i) == "true";

                    if (fvField(i) == "weight")
                        weights = fvValue(i);

                    if (fvField(i) == "nexthop_group")
                        nhg_index = fvValue(i);
                }

                /*
                 * A route should not fill both nexthop_group and ips /
                 * aliases.
                 */
                if (!nhg_index.empty() && (!ips.empty() || !aliases.empty()))
                {
                    SWSS_LOG_ERROR("Route %s has both nexthop_group and ips/aliases",
                                    key.c_str());
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                ctx.nhg_index = nhg_index;

                vector<string> ipv;
                vector<string> alsv;
                vector<string> mpls_nhv;

                /*
                 * If the nexthop_group is empty, create the next hop group key
                 * based on the IPs and aliases.  Otherwise, get the key from
                 * the NhgOrch.
                 */
                if (nhg_index.empty())
                {

                    ipv = tokenize(ips, ',');
                    alsv = tokenize(aliases, ',');
                    mpls_nhv = tokenize(mpls_nhs, ',');

                    /* Resize the ip vector to match ifname vector
                     * as tokenize(",", ',') will miss the last empty segment. */
                    if (alsv.size() == 0 && !blackhole)
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
                        if (removeLabelRoute(ctx))
                            it = consumer.m_toSync.erase(it);
                        else
                            it++;
                        continue;
                    }

                    string nhg_str = "";
                    NextHopGroupKey& nhg = ctx.nhg;

                    if (blackhole)
                    {
                        nhg = NextHopGroupKey();
                    }
                    else
                    {
                        for (uint32_t i = 0; i < ipv.size(); i++)
                        {
                            if (i) nhg_str += NHG_DELIMITER;
                            if (!mpls_nhv.empty() && mpls_nhv[i] != "na")
                            {
                                nhg_str += mpls_nhv[i] + LABELSTACK_DELIMITER;
                            }
                            nhg_str += ipv[i] + NH_DELIMITER + alsv[i];
                        }

                        nhg = NextHopGroupKey(nhg_str, weights);
                    }
                }
                else
                {
                    try
                    {
                        const NhgBase& nh_group = getNhg(nhg_index);
                        ctx.nhg = nh_group.getNhgKey();
                        ctx.using_temp_nhg = nh_group.isTemp();
                    }
                    catch (const std::out_of_range& e)
                    {
                        SWSS_LOG_ERROR("Next hop group %s does not exist", nhg_index.c_str());
                        ++it;
                        continue;
                    }
                }

                NextHopGroupKey& nhg = ctx.nhg;

                if (nhg.getSize() == 1 && nhg.hasIntfNextHop())
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
                    /* subnet route, vrf leaked route, etc */
                    else
                    {
                        if (addLabelRoute(ctx, nhg))
                            it = consumer.m_toSync.erase(it);
                        else
                            it++;
                    }
                }
                else if (m_syncdLabelRoutes.find(vrf_id) == m_syncdLabelRoutes.end() ||
                         m_syncdLabelRoutes.at(vrf_id).find(label) == m_syncdLabelRoutes.at(vrf_id).end() ||
                         m_syncdLabelRoutes.at(vrf_id).at(label) != RouteNhg(nhg, nhg_index) ||
                         ctx.using_temp_nhg)
                {
                    if (addLabelRoute(ctx, nhg))
                        it = consumer.m_toSync.erase(it);
                    else
                        it++;
                }
                else
                {
                    /* Duplicate entry */
                    SWSS_LOG_INFO("Route %s is duplicate entry", key.c_str());
                    it = consumer.m_toSync.erase(it);
                }

                // If already exhaust the nexthop groups, and there are pending removing routes in bulker,
                // flush the bulker and possibly collect some released nexthop groups
                if (m_nextHopGroupCount + NhgOrch::getSyncedNhgCount() >= m_maxNextHopGroupCount &&
                    gLabelRouteBulker.removing_entries_count() > 0)
                {
                    break;
                }
            }
            else if (op == DEL_COMMAND)
            {
                /* Cannot locate the route or remove succeed */
                if (removeLabelRoute(ctx))
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
        gLabelRouteBulker.flush();

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
            const Label& label = ctx.label;

            if (op == SET_COMMAND)
            {
                const bool& excp_intfs_flag = ctx.excp_intfs_flag;

                if (excp_intfs_flag)
                {
                    /* If any existing routes are updated to point to the
                     * above interfaces, remove them from the ASIC. */
                    if (removeLabelRoutePost(ctx))
                        it_prev = consumer.m_toSync.erase(it_prev);
                    else
                        it_prev++;
                    continue;
                }

                const NextHopGroupKey& nhg = ctx.nhg;

                if (nhg.getSize() == 1 && nhg.hasIntfNextHop())
                {
                    if (addLabelRoutePost(ctx, nhg))
                        it_prev = consumer.m_toSync.erase(it_prev);
                    else
                        it_prev++;
                }
                else if (m_syncdLabelRoutes.find(vrf_id) == m_syncdLabelRoutes.end() ||
                         m_syncdLabelRoutes.at(vrf_id).find(label) == m_syncdLabelRoutes.at(vrf_id).end() ||
                         m_syncdLabelRoutes.at(vrf_id).at(label) != RouteNhg(nhg, ctx.nhg_index) ||
                         ctx.using_temp_nhg)
                {
                    if (addLabelRoutePost(ctx, nhg))
                        it_prev = consumer.m_toSync.erase(it_prev);
                    else
                        it_prev++;
                }
            }
            else if (op == DEL_COMMAND)
            {
                /* Cannot locate the route or remove succeed */
                if (removeLabelRoutePost(ctx))
                    it_prev = consumer.m_toSync.erase(it_prev);
                else
                    it_prev++;
            }
        }

        /* Remove next hop group if the reference count decreases to zero */
        for (auto& it_nhg : m_bulkNhgReducedRefCnt)
        {
            if (m_syncdNextHopGroups[it_nhg.first].ref_count == 0)
            {
                removeNextHopGroup(it_nhg.first);
            }
        }
    }
}

void RouteOrch::addTempLabelRoute(LabelRouteBulkContext& ctx, const NextHopGroupKey &nextHops)
{
    SWSS_LOG_ENTER();

    Label& label = ctx.label;

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
            SWSS_LOG_INFO("Failed to get next hop %s for %u", (*it).to_string().c_str(), label);
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

    addLabelRoute(ctx, tmp_next_hop);
}

bool RouteOrch::addLabelRoute(LabelRouteBulkContext& ctx, const NextHopGroupKey &nextHops)
{
    SWSS_LOG_ENTER();

    sai_object_id_t& vrf_id = ctx.vrf_id;
    Label& label = ctx.label;

    /* next_hop_id indicates the next hop id or next hop group id of this route */
    sai_object_id_t next_hop_id = SAI_NULL_OBJECT_ID;
    bool blackhole = false;

    if (m_syncdLabelRoutes.find(vrf_id) == m_syncdLabelRoutes.end())
    {
        m_syncdLabelRoutes.emplace(vrf_id, LabelRouteTable());
        m_vrfOrch->increaseVrfRefCount(vrf_id);
    }

    auto it_route = m_syncdLabelRoutes.at(vrf_id).find(label);

    if (!ctx.nhg_index.empty())
    {
        try
        {
            const NhgBase& nhg = getNhg(ctx.nhg_index);
            next_hop_id = nhg.getId();
        }
        catch(const std::out_of_range& e)
        {
            SWSS_LOG_WARN("Next hop group key %s does not exist", ctx.nhg_index.c_str());
            return false;
        }
    }
    else if (nextHops.getSize() == 0)
    {
        /* The route is pointing to a blackhole */
        blackhole = true;
    }
    /* The route is pointing to a next hop */
    else if (nextHops.getSize() == 1)
    {
        const NextHopKey& nexthop = *nextHops.getNextHops().begin();
        if (nexthop.isIntfNextHop())
        {
            next_hop_id = m_intfsOrch->getRouterIntfsId(nexthop.alias);
            /* rif is not created yet */
            if (next_hop_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_INFO("Failed to get next hop %s for %u",
                        nextHops.to_string().c_str(), label);
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
                NeighborContext ctx = NeighborContext(nexthop);
                if (m_neighOrch->addNextHop(ctx))
                {
                    next_hop_id = m_neighOrch->getNextHopId(nexthop);
                }
                else
                {
                    return false;
                }
            }
            /* IP neighbor is not yet resolved */
            else
            {
                SWSS_LOG_INFO("Failed to get next hop %s for %u",
                        nextHops.to_string().c_str(), label);
                m_neighOrch->resolveNeighbor(nexthop);
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
                for (auto it = nextHops.getNextHops().begin(); it != nextHops.getNextHops().end(); ++it)
                {
                    const NextHopKey& nextHop = *it;
                    if (!m_neighOrch->hasNextHop(nextHop))
                    {
                        SWSS_LOG_INFO("Failed to get next hop %s in %s, resolving neighbor",
                                      nextHop.to_string().c_str(), nextHops.to_string().c_str());
                        m_neighOrch->resolveNeighbor(nextHop);
                    }
                }

                /* Failed to create the next hop group and check if a temporary route is needed */

                /* If the current next hop is part of the next hop group to sync,
                 * then return false and no need to add another temporary route. */
                if (it_route != m_syncdLabelRoutes.at(vrf_id).end() &&
                    it_route->second.nhg_key.getSize() == 1)
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
                addTempLabelRoute(ctx, nextHops);
                /* Return false since the original route is not successfully added */
                return false;
            }
        }

        next_hop_id = m_syncdNextHopGroups[nextHops].next_hop_group_id;
    }

    /* Sync the inseg entry */
    sai_inseg_entry_t inseg_entry;
    inseg_entry.switch_id = gSwitchId;
    inseg_entry.label = label;

    sai_attribute_t inseg_attr;
    auto& object_statuses = ctx.object_statuses;

    /* If the label is not in m_syncdLabelRoutes, then we need to create the route
     * for this prefix with the new next hop (group) id. If the prefix is already
     * in m_syncdLabelRoutes, then we need to update the route with a new next hop
     * (group) id. The old next hop (group) is then not used and the reference
     * count will decrease by 1.
     *
     * In case the entry is already pending removal in the bulk, it would be removed
     * from m_syncdLabelRoutes during the bulk call. Therefore, such entries need to be
     * re-created rather than set attribute.
     */
    if (it_route == m_syncdLabelRoutes.at(vrf_id).end() || gLabelRouteBulker.bulk_entry_pending_removal(inseg_entry))
    {
        vector<sai_attribute_t> inseg_attrs;
        if (blackhole)
        {
            inseg_attr.id = SAI_INSEG_ENTRY_ATTR_PACKET_ACTION;
            inseg_attr.value.s32 = SAI_PACKET_ACTION_DROP;
        }
        else
        {
            inseg_attr.id = SAI_INSEG_ENTRY_ATTR_NEXT_HOP_ID;
            inseg_attr.value.oid = next_hop_id;
        }
        inseg_attrs.push_back(inseg_attr);
        inseg_attr.id = SAI_INSEG_ENTRY_ATTR_NUM_OF_POP;
        inseg_attr.value.u32 = ctx.pop_count;
        inseg_attrs.push_back(inseg_attr);

        /* Default SAI_INSEG_ENTRY_ATTR_PACKET_ACTION is SAI_PACKET_ACTION_FORWARD */
        object_statuses.emplace_back();
        sai_status_t status = gLabelRouteBulker.create_entry(&object_statuses.back(), &inseg_entry, (uint32_t)inseg_attrs.size(), inseg_attrs.data());
        if (status == SAI_STATUS_ITEM_ALREADY_EXISTS)
        {
            SWSS_LOG_ERROR("Failed to create label route %u with next hop(s) %s",
                           label, nextHops.to_string().c_str());
            return false;
        }
    }
    else
    {
        /* Set the packet action to forward when there was no next hop (dropped) */
        if (it_route->second.nhg_key.getSize() == 0 && !blackhole)
        {
            inseg_attr.id = SAI_INSEG_ENTRY_ATTR_PACKET_ACTION;
            inseg_attr.value.s32 = SAI_PACKET_ACTION_FORWARD;

            object_statuses.emplace_back();
            gLabelRouteBulker.set_entry_attribute(&object_statuses.back(), &inseg_entry, &inseg_attr);
        }
        else if (blackhole)
        {
            inseg_attr.id = SAI_ROUTE_ENTRY_ATTR_PACKET_ACTION;
            inseg_attr.value.s32 = SAI_PACKET_ACTION_DROP;

            object_statuses.emplace_back();
            gLabelRouteBulker.set_entry_attribute(&object_statuses.back(), &inseg_entry, &inseg_attr);
        }
        else
        {
            inseg_attr.id = SAI_INSEG_ENTRY_ATTR_NEXT_HOP_ID;
            inseg_attr.value.oid = next_hop_id;

            /* Set the next hop ID to a new value */
            object_statuses.emplace_back();
            gLabelRouteBulker.set_entry_attribute(&object_statuses.back(), &inseg_entry, &inseg_attr);
        }
    }
    return false;
}

bool RouteOrch::addLabelRoutePost(const LabelRouteBulkContext& ctx, const NextHopGroupKey &nextHops)
{
    SWSS_LOG_ENTER();

    const sai_object_id_t& vrf_id = ctx.vrf_id;
    const Label& label = ctx.label;
    bool blackhole = false;

    const auto& object_statuses = ctx.object_statuses;

    if (object_statuses.empty())
    {
        // Something went wrong before router bulker, will retry
        return false;
    }

    /* next_hop_id indicates the next hop id or next hop group id of this route */
    sai_object_id_t next_hop_id;

    /* Check that the next hop group is not owned by NhgOrch. */
    if (!ctx.nhg_index.empty())
    {
        if (!gNhgOrch->hasNhg(ctx.nhg_index) && !gCbfNhgOrch->hasNhg(ctx.nhg_index))
        {
            SWSS_LOG_WARN("Failed to get next hop group with index %s", ctx.nhg_index.c_str());
            return false;
        }
    }
    else if (nextHops.getSize() == 0)
    {
        /* The route is pointing to a blackhole */
        blackhole = true;
    }
    /* The route is pointing to a next hop */
    else if (nextHops.getSize() == 1)
    {
        const NextHopKey& nexthop = *nextHops.getNextHops().begin();
        if (nexthop.isIntfNextHop())
        {

            next_hop_id = m_intfsOrch->getRouterIntfsId(nexthop.alias);
            /* rif is not created yet */
            if (next_hop_id == SAI_NULL_OBJECT_ID)
            {
                SWSS_LOG_INFO("Failed to get next hop %s for label %u",
                              nextHops.to_string().c_str(), label);
                return false;
            }
        }
        else
        {
            if (!m_neighOrch->hasNextHop(nexthop))
            {
                SWSS_LOG_INFO("Failed to get next hop %s for label %u",
                              nextHops.to_string().c_str(), label);
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
            addLabelRoutePost(ctx, tmp_next_hop);
            return false;
        }
    }

    auto it_status = object_statuses.begin();
    auto it_route = m_syncdLabelRoutes.at(vrf_id).find(label);
    if (it_route == m_syncdLabelRoutes.at(vrf_id).end())
    {
        if (*it_status++ != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create label %u with next hop(s) %s",
                           label, nextHops.to_string().c_str());
            /* Clean up the newly created next hop group entry */
            if (nextHops.getSize() > 1)
            {
                removeNextHopGroup(nextHops);
            }
            return false;
        }

        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_MPLS_INSEG);

        /* Increase the ref_count for the next hop (group) entry */
        if (ctx.nhg_index.empty())
        {
            increaseNextHopRefCount(nextHops);
        }
        else
        {
            incNhgRefCount(ctx.nhg_index);
        }

        SWSS_LOG_INFO("Post create label %u with next hop(s) %s",
                      label, nextHops.to_string().c_str());
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
                SWSS_LOG_ERROR("Failed to set label %u with packet action forward, %d",
                               label, status);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_MPLS, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
        }

        status = *it_status++;
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to set label %u with next hop(s) %s",
                           label, nextHops.to_string().c_str());
            task_process_status handle_status = handleSaiSetStatus(SAI_API_MPLS, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        /* Decrease the ref count for the previous next hop group. */
        if (it_route->second.nhg_index.empty())
        {
            decreaseNextHopRefCount(it_route->second.nhg_key);
            if (it_route->second.nhg_key.getSize() > 1
                && m_syncdNextHopGroups[it_route->second.nhg_key].ref_count == 0)
            {
                m_bulkNhgReducedRefCnt.emplace(it_route->second.nhg_key, 0);
            }
        }
        /* The next hop group is owned by (Cbf)NhgOrch. */
        else
        {
            decNhgRefCount(it_route->second.nhg_index);
        }

        /* Increase the ref_count for the next hop (group) entry */
        if (ctx.nhg_index.empty())
        {
            increaseNextHopRefCount(nextHops);
        }
        else
        {
            incNhgRefCount(ctx.nhg_index);
        }

        if (blackhole)
        {
            /* Set the packet action to drop for blackhole routes */
            status = *it_status++;
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to set blackhole label %u with packet action drop, %d",
                                label, status);
                task_process_status handle_status = handleSaiSetStatus(SAI_API_MPLS, status);
                if (handle_status != task_success)
                {
                    return parseHandleSaiStatusFailure(handle_status);
                }
            }
        }

        SWSS_LOG_INFO("Post set label %u with next hop(s) %s",
                      label, nextHops.to_string().c_str());
    }

    m_syncdLabelRoutes[vrf_id][label] = RouteNhg(nextHops, ctx.nhg_index);

    return true;
}

bool RouteOrch::removeLabelRoute(LabelRouteBulkContext& ctx)
{
    SWSS_LOG_ENTER();

    sai_object_id_t& vrf_id = ctx.vrf_id;
    Label& label = ctx.label;

    auto it_route_table = m_syncdLabelRoutes.find(vrf_id);
    if (it_route_table == m_syncdLabelRoutes.end())
    {
        SWSS_LOG_INFO("Failed to find route table, vrf_id 0x%" PRIx64 "\n", vrf_id);
        return true;
    }

    sai_inseg_entry_t inseg_entry;
    inseg_entry.switch_id = gSwitchId;
    inseg_entry.label = label;

    auto it_route = it_route_table->second.find(label);
    //size_t creating = gLabelRouteBulker.creating_entries_count(inseg_entry);
    if (it_route == it_route_table->second.end())
    {
        SWSS_LOG_INFO("Failed to find inseg entry, vrf_id 0x%" PRIx64 ", label %u\n",
                      vrf_id, label);
        return true;
    }

    auto& object_statuses = ctx.object_statuses;

    object_statuses.emplace_back();
    gLabelRouteBulker.remove_entry(&object_statuses.back(), &inseg_entry);

    return false;
}

bool RouteOrch::removeLabelRoutePost(const LabelRouteBulkContext& ctx)
{
    SWSS_LOG_ENTER();

    const sai_object_id_t& vrf_id = ctx.vrf_id;
    const Label& label = ctx.label;

    auto& object_statuses = ctx.object_statuses;

    if (object_statuses.empty())
    {
        // Something went wrong before router bulker, will retry
        return false;
    }

    auto it_route_table = m_syncdLabelRoutes.find(vrf_id);
    auto it_route = it_route_table->second.find(label);
    auto it_status = object_statuses.begin();

    sai_status_t status = *it_status++;
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove label:%u\n", label);
        task_process_status handle_status = handleSaiRemoveStatus(SAI_API_MPLS, status);
        if (handle_status != task_success)
        {
            return parseHandleSaiStatusFailure(handle_status);
        }
    }

    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_MPLS_INSEG);

    if (it_route->second.nhg_index.empty())
    {
        /*
         * Decrease the reference count only when the route is pointing to a next hop.
         */
        decreaseNextHopRefCount(it_route->second.nhg_key);
        if (it_route->second.nhg_key.getSize() > 1
            && m_syncdNextHopGroups[it_route->second.nhg_key].ref_count == 0)
        {
            m_bulkNhgReducedRefCnt.emplace(it_route->second.nhg_key, 0);
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
        }
    }
    else
    {
        decNhgRefCount(it_route->second.nhg_index);
    }

    SWSS_LOG_INFO("Remove label route %u with next hop(s) %s",
                  label, it_route->second.nhg_key.to_string().c_str());

    it_route_table->second.erase(label);

    if (it_route_table->second.size() == 0)
    {
        m_syncdLabelRoutes.erase(vrf_id);
        m_vrfOrch->decreaseVrfRefCount(vrf_id);
    }

    return true;
}
