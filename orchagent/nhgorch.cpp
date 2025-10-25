#include "nhgorch.h"
#include "neighorch.h"
#include "crmorch.h"
#include "routeorch.h"
#include "srv6orch.h"
#include "bulker.h"
#include "logger.h"
#include "swssnet.h"

extern sai_object_id_t gSwitchId;

extern IntfsOrch *gIntfsOrch;
extern NeighOrch *gNeighOrch;
extern RouteOrch *gRouteOrch;
extern NhgOrch *gNhgOrch;
extern Srv6Orch *gSrv6Orch;

extern size_t gMaxBulkSize;

extern sai_next_hop_group_api_t* sai_next_hop_group_api;
extern sai_next_hop_api_t*         sai_next_hop_api;

NhgOrch::NhgOrch(DBConnector *db, string tableName) : NhgOrchCommon(db, tableName)
{
    SWSS_LOG_ENTER();
    createRetryCache(tableName);
}

/*
 * Purpose:     Perform the operations requested by APPL_DB users.
 * Description: Iterate over the untreated operations list and resolve them.
 *              The operations supported are SET and DEL.  If an operation
 *              could not be resolved, it will either remain in the list, or be
 *              removed, depending on the case.
 * Params:      IN  consumer - The cosumer object.
 * Returns:     Nothing.
 */
void NhgOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple t = it->second;

        string index = kfvKey(t);
        string op = kfvOp(t);

        bool success = false;
        const auto& nhg_it = m_syncdNextHopGroups.find(index);

        if (op == SET_COMMAND)
        {
            string ips;
            string aliases;
            string weights;
            string mpls_nhs;
            string nhgs;
            bool is_recursive = false;
            string srv6_source;
            bool overlay_nh = false;
            bool srv6_nh = false;

            /* Get group's next hop IPs and aliases */
            for (auto i : kfvFieldsValues(t))
            {
                if (fvField(i) == "nexthop" && fvValue(i) != "")
                    ips = fvValue(i);

                if (fvField(i) == "ifname" && fvValue(i) != "")
                    aliases = fvValue(i);

                if (fvField(i) == "weight" && fvValue(i) != "")
                    weights = fvValue(i);

                if (fvField(i) == "mpls_nh" && fvValue(i) != "")
                    mpls_nhs = fvValue(i);

                if (fvField(i) == "seg_src" && fvValue(i) != "")
                {
                    srv6_source = fvValue(i);
                    srv6_nh = true;
                }

                if (fvField(i) == "nexthop_group" && fvValue(i) != "")
                {
                    nhgs = fvValue(i);
                    is_recursive = true;
                }
            }
            /* A NHG should not have both regular(ip/alias) and recursive fields */
            if (is_recursive && (!ips.empty() || !aliases.empty()))
            {
                SWSS_LOG_ERROR("Nexthop group %s has both regular(ip/alias) and recursive fields", index.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }
            /* Split ips and aliases strings into vectors of tokens. */
            vector<string> ipv = tokenize(ips, ',');
            vector<string> alsv = tokenize(aliases, ',');
            vector<string> mpls_nhv = tokenize(mpls_nhs, ',');
            vector<string> nhgv = tokenize(nhgs, NHG_DELIMITER);
            vector<string> srv6_srcv = tokenize(srv6_source, ',');

            /* Create the next hop group key. */
            string nhg_str;
            NextHopGroupKey nhg_key;

            /* Keeps track of any non-existing member of a recursive nexthop group */
            bool non_existent_member = false;

            if (is_recursive)
            {
                SWSS_LOG_INFO("Adding recursive nexthop group %s with %s", index.c_str(), nhgs.c_str());

                /* Reset the "nexthop_group" field and update it with only the existing members */
                nhgs = "";

                /* Check if any of the members are a recursive or temporary nexthop group */
                bool invalid_member = false;

                for (auto& nhgm : nhgv)
                {
                    const auto& nhgm_it = m_syncdNextHopGroups.find(nhgm);
                    if (nhgm_it == m_syncdNextHopGroups.end())
                    {
                        SWSS_LOG_INFO("Member nexthop group %s in parent nhg %s not ready",
                                nhgm.c_str(), index.c_str());

                        non_existent_member = true;
                        continue;
                    }
                    if ((nhgm_it->second.nhg) &&
                        (nhgm_it->second.nhg->isRecursive() || nhgm_it->second.nhg->isTemp()))
                    {
                        SWSS_LOG_ERROR("Invalid member nexthop group %s in parent nhg %s",
                                nhgm.c_str(), index.c_str());

                        invalid_member = true;
                        break;
                    }
                    /* Keep only the members which exist in the local cache */
                    if (nhgs.empty())
                        nhgs = nhgm;
                    else
                        nhgs += NHG_DELIMITER + nhgm;
                }
                if (invalid_member)
                {
                    it = consumer.m_toSync.erase(it);
                    continue;
                }
                /* If no members are present */
                if (nhgs.empty())
                {
                    it++;
                    continue;
                }

                /* Form nexthopgroup key with the nexthopgroup keys of available members */
                nhgv = tokenize(nhgs, NHG_DELIMITER);

                bool nhg_mismatch = false;
                for (uint32_t i = 0; i < nhgv.size(); i++)
                {
                    auto k = m_syncdNextHopGroups.at(nhgv[i]).nhg->getKey();
                    if (i)
                    {
                        if (k.is_srv6_nexthop() != srv6_nh || k.is_overlay_nexthop() != overlay_nh)
                        {
                            SWSS_LOG_ERROR("Inconsistent nexthop group type between %s and %s",
                                m_syncdNextHopGroups.at(nhgv[0]).nhg->getKey().to_string().c_str(),
                                k.to_string().c_str());
                            nhg_mismatch = true;
                            break;
                        }
                        nhg_str += NHG_DELIMITER;
                    }
                    else
                    {
                        srv6_nh = k.is_srv6_nexthop();
                        overlay_nh = k.is_overlay_nexthop();
                    }

                    nhg_str += m_syncdNextHopGroups.at(nhgv[i]).nhg->getKey().to_string();
                }

                if (nhg_mismatch)
                {
                    it = consumer.m_toSync.erase(it);
                    continue;
                }

                if (srv6_nh)
                    nhg_key = NextHopGroupKey(nhg_str, overlay_nh, srv6_nh);
                else
                    nhg_key = NextHopGroupKey(nhg_str, weights);
            }
            else
            {
                if (srv6_nh)
                {
                    if (ipv.size() != srv6_srcv.size())
                    {
                        SWSS_LOG_ERROR("inconsistent number of endpoints and srv6_srcs.");
                        it = consumer.m_toSync.erase(it);
                        continue;
                    }
                    for (uint32_t i = 0; i < ipv.size(); i++)
                    {
                        if (i) nhg_str += NHG_DELIMITER;
                        nhg_str += ipv[i] + NH_DELIMITER;       // ip address
                        nhg_str += NH_DELIMITER;                // srv6 segment
                        nhg_str += srv6_srcv[i] + NH_DELIMITER; // srv6 source
                        nhg_str += NH_DELIMITER;                // srv6 vpn sid
                    }
                    nhg_key = NextHopGroupKey(nhg_str, overlay_nh, srv6_nh);
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
                    nhg_key = NextHopGroupKey(nhg_str, weights);
                }
            }

            /* If the group does not exist, create one. */
            if (nhg_it == m_syncdNextHopGroups.end())
            {
                SWSS_LOG_INFO("Create nexthop group %s with %s", index.c_str(), nhg_str.c_str());

                /*
                * If we've reached the NHG limit, we're going to create a temporary
                * group, represented by one of it's NH only until we have
                * enough resources to sync the whole group.  The item is going
                * to be kept in the sync list so we keep trying to create the
                * actual group when there are enough resources.
                */
                if (gRouteOrch->getNhgCount() + NextHopGroup::getSyncedCount() >= gRouteOrch->getMaxNhgCount())
                {
                    SWSS_LOG_DEBUG("Next hop group count reached its limit.");

                    // don't create temp nhg for srv6
                    if (nhg_key.is_srv6_nexthop())
                    {
                        ++it;
                        continue;
                    }

                    try
                    {
                        auto nhg = std::make_unique<NextHopGroup>(createTempNhg(nhg_key));
                        if (nhg->sync())
                        {
                            m_syncdNextHopGroups.emplace(index, NhgEntry<NextHopGroup>(std::move(nhg)));
                            notifyRetry(gRouteOrch, APP_ROUTE_TABLE_NAME, make_constraint(RETRY_CST_NHG, index));
                        }
                        else
                        {
                            SWSS_LOG_INFO("Failed to sync temporary NHG %s with %s",
                                index.c_str(),
                                nhg_key.to_string().c_str());
                        }
                    }
                    catch (const std::exception& e)
                    {
                        SWSS_LOG_INFO("Got exception: %s while adding temp group %s",
                            e.what(),
                            nhg_key.to_string().c_str());
                    }
                }
                else
                {
                    auto nhg = std::make_unique<NextHopGroup>(nhg_key, false);

                    /* 
                    * Mark the nexthop group as recursive so as to create a
                    * nexthop group object even if it has just one available path
                    */
                    nhg->setRecursive(is_recursive);

                    success = nhg->sync();
                    if (success)
                    {
                        /* Keep the msg in loop if any member path is not available yet */
                        if (is_recursive && non_existent_member)
                        {
                            success = false;
                        }
                        m_syncdNextHopGroups.emplace(index, NhgEntry<NextHopGroup>(std::move(nhg)));
                        notifyRetry(gRouteOrch, APP_ROUTE_TABLE_NAME, make_constraint(RETRY_CST_NHG, index));
                    }
                }
            }
            /* If the group exists, update it. */
            else
            {
                SWSS_LOG_INFO("Update nexthop group %s with %s", index.c_str(), nhg_str.c_str());

                const auto& nhg_ptr = nhg_it->second.nhg;

                /*
                 * If the update would mandate promoting a temporary next hop
                 * group to a multiple next hops group and we do not have the
                 * resources yet, we have to skip it until we have enough
                 * resources.
                 */
                if (nhg_ptr->isTemp() &&
                    (gRouteOrch->getNhgCount() + NextHopGroup::getSyncedCount() >= gRouteOrch->getMaxNhgCount()))
                {
                    /*
                     * If the group was updated in such way that the previously
                     * chosen next hop does not represent the new group key,
                     * update the temporary group to choose a new next hop from
                     * the new key.  Otherwise, this will be a no-op as we have
                     * to wait for resources in order to promote the group.
                     */
                    if (!nhg_key.contains(nhg_ptr->getKey()))
                    {
                        try
                        {
                            /* Create the new temporary next hop group. */
                            auto new_nhg = std::make_unique<NextHopGroup>(createTempNhg(nhg_key));

                            /*
                            * If we successfully sync the new group, update
                            * only the next hop group entry's pointer so we
                            * don't mess up the reference counter, as other
                            * objects may already reference it.
                            */
                            if (new_nhg->sync())
                            {
                                nhg_it->second.nhg = std::move(new_nhg);
                            }
                            else
                            {
                                SWSS_LOG_INFO("Failed to sync updated temp NHG %s with %s",
                                  index.c_str(),
                                  nhg_key.to_string().c_str());
                            }
                        }
                        catch (const std::exception& e)
                        {
                            SWSS_LOG_INFO("Got exception: %s while adding temp group %s",
                                e.what(),
                                nhg_key.to_string().c_str());
                        }
                    }
                }
                /*
                 * If the group is temporary but can now be promoted, create and sync a new group for
                 * the desired next hops.
                 */
                else if (nhg_ptr->isTemp())
                {
                    auto nhg = std::make_unique<NextHopGroup>(nhg_key, false);
                    success = nhg->sync();

                    if (success)
                    {
                        /*
                         * Placing the new group in the map will replace the temporary group, causing
                         * it to be removed and freed.
                         */
                        nhg_it->second.nhg = std::move(nhg);
                    }
                }
                /* Common update, when all the requirements are met. */
                else
                {
                    success = nhg_ptr->update(nhg_key);

                    /* Keep the msg in loop if any member path is not available yet */
                    if (is_recursive && non_existent_member)
                    {
                        success = false;
                    }
                }
            }
        }
        else if (op == DEL_COMMAND)
        {
            /*
             * If there is a pending SET after this DEL operation, skip the
             * DEL operation to perform the update instead.  Otherwise, in the
             * scenario where the DEL operation may be blocked by the ref
             * counter, we'd end up deleting the object after the SET operation
             * is performed, which would not reflect the desired state of the
             * object.
             */
            if (consumer.m_toSync.count(it->first) > 1)
            {
                success = true;
            }
            /* If the group does not exist, do nothing. */
            else if (nhg_it == m_syncdNextHopGroups.end())
            {
                SWSS_LOG_INFO("Unable to find group with key %s to remove", index.c_str());
                /* Mark the operation as successful to consume it. */
                success = true;
            }
            /* If the group does exist, but it's still referenced, skip. */
            else if (nhg_it->second.ref_count > 0)
            {
                SWSS_LOG_INFO("Unable to remove group %s which is referenced", index.c_str());
                success = consumer.addToRetry(std::move(it->second), make_constraint(RETRY_CST_NHG_REF, index));
            }
            /* Else, if the group is no more referenced, remove it. */
            else
            {
                const auto& nhg = nhg_it->second.nhg;

                success = nhg->remove();

                if (success)
                {
                    m_syncdNextHopGroups.erase(nhg_it);
                    notifyRetry(gRouteOrch, APP_ROUTE_TABLE_NAME, make_constraint(RETRY_CST_ECMP));
                }
            }
        }
        else
        {
            SWSS_LOG_ERROR("Unknown operation type %s\n", op.c_str());
            /* Mark the operation as successful to consume it. */
            success = true;
        }

        /* Depending on the operation success, consume it or skip it. */
        if (success)
        {
            it = consumer.m_toSync.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

/*
 * Purpose:     Validate a next hop for any groups that contains it.
 * Description: Iterate over all next hop groups and validate the next hop in
 *              those who contain it.
 * Params:      IN  nh_key - The next hop to validate.
 * Returns:     true, if the next hop was successfully validated in all
 *              containing groups;
 *              false, otherwise.
 */
bool NhgOrch::validateNextHop(const NextHopKey& nh_key)
{
    SWSS_LOG_ENTER();

    /*
     * Iterate through all groups and validate the next hop in those who
     * contain it.
     */
    for (auto& it : m_syncdNextHopGroups)
    {
        auto& nhg = it.second.nhg;

        if (nhg->hasMember(nh_key))
        {
            /*
             * If sync fails, exit right away, as we expect it to be due to a
             * raeson for which any other future validations will fail too.
             */
            if (!nhg->validateNextHop(nh_key))
            {
                SWSS_LOG_ERROR("Failed to validate next hop %s in group %s",
                                nh_key.to_string().c_str(),
                                it.first.c_str());
                return false;
            }
        }
    }

    return true;
}

/*
 * Purpose:     Invalidate a next hop for any groups containing it.
 * Description: Iterate through the next hop groups and remove the next hop
 *              from those that contain it.
 * Params:      IN  nh_key - The next hop to invalidate.
 * Returns:     true, if the next hop was successfully invalidatedd from all
 *              containing groups;
 *              false, otherwise.
 */
bool NhgOrch::invalidateNextHop(const NextHopKey& nh_key)
{
    SWSS_LOG_ENTER();

    /*
     * Iterate through all groups and invalidate the next hop from those who
     * contain it.
     */
    for (auto& it : m_syncdNextHopGroups)
    {
        auto& nhg = it.second.nhg;

        if (nhg->hasMember(nh_key))
        {
            /* If the remove fails, exit right away. */
            if (!nhg->invalidateNextHop(nh_key))
            {
                SWSS_LOG_WARN("Failed to invalidate next hop %s from group %s",
                                nh_key.to_string().c_str(),
                                it.first.c_str());
                return false;
            }
        }
    }

    return true;
}

/*
 * Purpose:     Get the next hop ID of the member.
 * Description: Get the SAI ID of the next hop from NeighOrch.
 * Params:      None.
 * Returns:     The SAI ID of the next hop, or SAI_NULL_OBJECT_ID if the next
 *              hop is not valid.
 */
sai_object_id_t NextHopGroupMember::getNhId() const
{
    SWSS_LOG_ENTER();

    sai_object_id_t nh_id = SAI_NULL_OBJECT_ID;

    if (m_key.isIntfNextHop())
    {
        nh_id = gIntfsOrch->getRouterIntfsId(m_key.alias);
    }
    else if (gNeighOrch->hasNextHop(m_key))
    {
        nh_id = gNeighOrch->getNextHopId(m_key);
        if (m_key.isSrv6NextHop())
        {
            SWSS_LOG_INFO("Single NH: create srv6 nexthop %s", m_key.to_string(false, true).c_str());
            if (!gSrv6Orch->createSrv6NexthopWithoutVpn(m_key, nh_id))
            {
                SWSS_LOG_ERROR("Failed to create SRv6 nexthop %s", m_key.to_string(false, true).c_str());
            }
        }
    }
    /*
     * If the next hop is labeled and the IP next hop exists, create the
     * labeled one over NeighOrch as it doesn't know about these next hops.
     * We don't do this in the constructor as the IP next hop may be added
     * after the object is created and would never create the labeled next hop
     * afterwards.
     */
    else if (isLabeled() && gNeighOrch->isNeighborResolved(m_key))
    {
        NeighborContext ctx = NeighborContext(m_key);
        if (gNeighOrch->addNextHop(ctx))
        {
            nh_id = gNeighOrch->getNextHopId(m_key);
        }
    }
    else
    {
        if (m_key.isSrv6NextHop())
        {
            SWSS_LOG_INFO("Single NH: create srv6 nexthop %s", m_key.to_string(false, true).c_str());
            if (!gSrv6Orch->createSrv6NexthopWithoutVpn(m_key, nh_id))
            {
                SWSS_LOG_ERROR("Failed to create SRv6 nexthop %s", m_key.to_string(false, true).c_str());
            }
        }
        else
        {
            SWSS_LOG_INFO("Failed to get next hop %s, resolving neighbor",
                m_key.to_string().c_str());
            gNeighOrch->resolveNeighbor(m_key);
        }
    }

    return nh_id;
}

/*
 * Purpose:     Update the weight of a member.
 * Description: Set the new member's weight and if the member is synced, update
 *              the SAI attribute as well.
 * Params:      IN  weight - The weight of the next hop group member.
 * Returns:     true, if the operation was successful;
 *              false, otherwise.
 */
bool NextHopGroupMember::updateWeight(uint32_t weight)
{
    SWSS_LOG_ENTER();

    bool success = true;

    m_key.weight = weight;

    if (isSynced())
    {
        sai_attribute_t nhgm_attr;
        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT;
        nhgm_attr.value.s32 = m_key.weight;

        sai_status_t status = sai_next_hop_group_api->set_next_hop_group_member_attribute(m_gm_id, &nhgm_attr);
        success = status == SAI_STATUS_SUCCESS;
    }

    return success;
}

/*
 * Purpose:     Sync the group member with the given group member ID.
 * Description: Set the group member's SAI ID to the the one given and
 *              increment the appropriate ref counters.
 * Params:      IN  gm_id - The group member SAI ID to set.
 * Returns:     Nothing.
 */
void NextHopGroupMember::sync(sai_object_id_t gm_id)
{
    SWSS_LOG_ENTER();

    NhgMember::sync(gm_id);
    gNeighOrch->increaseNextHopRefCount(m_key);
}

/*
 * Purpose:     Remove the group member, resetting it's SAI ID.
 * Description: Reset the group member's SAI ID and decrement the appropriate
 *              ref counters.
 * Params:      None.
 * Returns:     Nothing.
 */
void NextHopGroupMember::remove()
{
    SWSS_LOG_ENTER();

    NhgMember::remove();
    gNeighOrch->decreaseNextHopRefCount(m_key);
}

/*
 * Purpose:     Destructor.
 * Description: Assert the group member is removed and remove the labeled
 *              next hop from NeighOrch if it is unreferenced.
 * Params:      None.
 * Returns:     Nothing.
 */
NextHopGroupMember::~NextHopGroupMember()
{
    SWSS_LOG_ENTER();

    if (m_key.isSrv6NextHop() && gNeighOrch->hasNextHop(m_key) &&
                !gNeighOrch->getNextHopRefCount(m_key))
    {
        if (!gSrv6Orch->removeSrv6NexthopWithoutVpn(m_key))
        {
            SWSS_LOG_ERROR("SRv6 Nexthop %s delete failed", m_key.to_string(false, true).c_str());
        }
    }
    /*
     * If the labeled next hop is unreferenced, remove it from NeighOrch as
     * NhgOrch and RouteOrch are the ones controlling it's lifetime.  They both
     * watch over these labeled next hops, so it doesn't matter who created
     * them as they're both doing the same checks before removing a labeled
     * next hop.
     */
    else if (isLabeled() &&
        gNeighOrch->hasNextHop(m_key) &&
        (gNeighOrch->getNextHopRefCount(m_key) == 0))
    {
        gNeighOrch->removeMplsNextHop(m_key);
    }
}

/*
 * Purpose:     Constructor.
 * Description: Initialize the group's members based on the next hop group key.
 * Params:      IN  key - The next hop group's key.
 * Returns:     Nothing.
 */
NextHopGroup::NextHopGroup(const NextHopGroupKey& key, bool is_temp) : NhgCommon(key), m_is_temp(is_temp), m_is_recursive(false)
{
    SWSS_LOG_ENTER();

    /* Parse the key and create the members. */
    for (const auto& it : m_key.getNextHops())
    {
        m_members.emplace(it, NextHopGroupMember(it));
    }
}

/*
 * Purpose:     Move assignment operator.
 * Description: Perform member-wise swap.
 * Params:      IN  nhg - The rvalue object to swap with.
 * Returns:     Referene to this object.
 */
NextHopGroup& NextHopGroup::operator=(NextHopGroup&& nhg)
{
    SWSS_LOG_ENTER();

    m_is_temp = nhg.m_is_temp;
    m_is_recursive = nhg.m_is_recursive;

    NhgCommon::operator=(std::move(nhg));

    return *this;
}

/*
 * Purpose:     Sync a next hop group.
 * Description: Fill in the NHG ID.  If the group contains only one NH, this ID
 *              will be the SAI ID of the next hop that NeighOrch owns.  If it
 *              has more than one NH, create a group over the SAI API and then
 *              add it's members.
 * Params:      None.
 * Returns:     true, if the operation was successful;
 *              false, otherwise.
 */
bool NextHopGroup::sync()
{
    SWSS_LOG_ENTER();

    /* If the group is already synced, exit. */
    if (isSynced())
    {
        return true;
    }

    /* If the group is non-recursive with single member, the group ID will be the only member's NH ID */
    if (!isRecursive() && (m_members.size() == 1))
    {
        const NextHopGroupMember& nhgm = m_members.begin()->second;
        sai_object_id_t nhid = nhgm.getNhId();

        if (nhid == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("Next hop %s is not synced", nhgm.getKey().to_string().c_str());
            return false;
        }
        else
        {
            m_id = nhid;

            auto nh_key = nhgm.getKey();
            if (nh_key.isIntfNextHop())
                gIntfsOrch->increaseRouterIntfsRefCount(nh_key.alias);
            else
                gNeighOrch->increaseNextHopRefCount(nh_key);
        }
    }
    else
    {
        /* Assert the group is not empty. */
        assert(!m_members.empty());

        /* Create the group over SAI. */
        sai_attribute_t nhg_attr;
        vector<sai_attribute_t> nhg_attrs;

        nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
        nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_ECMP;
        nhg_attrs.push_back(nhg_attr);

        sai_status_t status = sai_next_hop_group_api->create_next_hop_group(
                                                    &m_id,
                                                    gSwitchId,
                                                    (uint32_t)nhg_attrs.size(),
                                                    nhg_attrs.data());

        /* If the operation fails, exit. */
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create next hop group %s, rv:%d",
                            m_key.to_string().c_str(), status);

            task_process_status handle_status = handleSaiCreateStatus(SAI_API_NEXT_HOP_GROUP, status);
            if (handle_status != task_success)
            {
                return parseHandleSaiStatusFailure(handle_status);
            }
        }

        /* Increment the amount of programmed next hop groups. */
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);

        /* Increment the number of synced NHGs. */
        ++m_syncdCount;

        /*
        * Try creating the next hop group's members over SAI.
        */
        if (!syncMembers(m_key.getNextHops()))
        {
            SWSS_LOG_WARN("Failed to create next hop members of group %s",
                            to_string().c_str());
            return false;
        }
    }

    return true;
}

/*
 * Purpose:     Create a temporary next hop group when resources are exhausted.
 * Description: Choose one member to represent the group and create a group
 *              with only that next hop as a member.  Any object referencing
 *              the SAI ID of a temporary group should keep querying NhgOrch in
 *              case the group is updated, as it's SAI ID will change at that
 *              point.
 * Params:      IN  index   - The CP index of the next hop group.
 * Returns:     The created temporary next hop group.
 */
NextHopGroup NhgOrch::createTempNhg(const NextHopGroupKey& nhg_key)
{
    SWSS_LOG_ENTER();

    /* Get a list of all valid next hops in the group. */
    std::list<NextHopKey> valid_nhs;

    for (const auto& nh_key : nhg_key.getNextHops())
    {
        /*
         * Check if the IP next hop exists.  We check for the IP next hop as
         * the group might contain labeled NHs which we should create if their
         * IP next hop does exist.
         */
        if (gNeighOrch->isNeighborResolved(nh_key))
        {
            valid_nhs.push_back(nh_key);
        }
    }

    /* If there is no valid member, exit. */
    if (valid_nhs.empty())
    {
        SWSS_LOG_INFO("There is no valid NH to sync temporary group %s",
                        nhg_key.to_string().c_str());
        throw std::logic_error("No valid NH in the key");
    }

    /* Randomly select the valid NH to represent the group. */
    auto it = valid_nhs.begin();
    advance(it, rand() % valid_nhs.size());

    /* Create the temporary group. */
    NextHopGroup nhg(NextHopGroupKey(it->to_string()), true);

    return nhg;
}

/*
 * Purpose:     Remove the next hop group.
 * Description: Reset the group's SAI ID.  If the group has more than one
 *              members, remove the members and the group.
 * Params:      None.
 * Returns:     true, if the operation was successful;
 *              false, otherwise
 */
bool NextHopGroup::remove()
{
    SWSS_LOG_ENTER();

    if (!isSynced())
    {
        return true;
    }
    //  If the group is temporary or non-recursive, update the neigh or rif ref-count and reset the ID.
    if (m_is_temp ||
        (!isRecursive() && m_members.size() == 1))
    {
        const NextHopGroupMember& nhgm = m_members.begin()->second;
        auto nh_key = nhgm.getKey();
        if (nh_key.isIntfNextHop())
            gIntfsOrch->decreaseRouterIntfsRefCount(nh_key.alias);
        else
            gNeighOrch->decreaseNextHopRefCount(nh_key);

        m_id = SAI_NULL_OBJECT_ID;
        return true;
    }

    return NhgCommon::remove();
}

/*
 * Purpose:     Sync the given next hop group's members over the SAI API.
 * Description: Iterate over the given members and sync them.  If the member
 *              is already synced, we skip it.  If any of the next hops isn't
 *              already synced by the neighOrch, this will fail.  Any next hop
 *              which has the neighbor interface down will be skipped.
 * Params:      IN  nh_keys - The next hop keys of the members to sync.
 * Returns:     true, if the members were added succesfully;
 *              false, otherwise.
 */
bool NextHopGroup::syncMembers(const std::set<NextHopKey>& nh_keys)
{
    SWSS_LOG_ENTER();

    /* This method should not be called for single-membered non-recursive nexthop groups */
    assert(isRecursive() || (m_members.size() > 1));

    ObjectBulker<sai_next_hop_group_api_t> nextHopGroupMemberBulker(sai_next_hop_group_api, gSwitchId, gMaxBulkSize);

    /*
     * Iterate over the given next hops.
     * If the group member is already synced, skip it.
     * If any next hop is not synced, thus neighOrch doesn't have it, stop
     * immediately.
     * If a next hop's interface is down, skip it from being synced.
     */
    std::map<NextHopKey, sai_object_id_t> syncingMembers;

    bool success = true;
    for (const auto& nh_key : nh_keys)
    {
        NextHopGroupMember& nhgm = m_members.at(nh_key);

        /* If the member is already synced, continue. */
        if (nhgm.isSynced())
        {
            continue;
        }

        /*
         * If the next hop doesn't exist, stop from syncing the members.
         */
        if (nhgm.getNhId() == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("Failed to get next hop %s in group %s",
                        nhgm.to_string().c_str(), to_string().c_str());
            success = false;
            continue;
        }

        /* If the neighbor's interface is down, skip from being syncd. */
        if (gNeighOrch->isNextHopFlagSet(nh_key, NHFLAGS_IFDOWN))
        {
            SWSS_LOG_WARN("Skip next hop %s in group %s, interface is down",
                        nh_key.to_string().c_str(), to_string().c_str());
            continue;
        }

        /* Create the next hop group member's attributes and fill them. */
        vector<sai_attribute_t> nhgm_attrs = createNhgmAttrs(nhgm);

        /* Add a bulker entry for this member. */
        nextHopGroupMemberBulker.create_entry(&syncingMembers[nh_key],
                                               (uint32_t)nhgm_attrs.size(),
                                               nhgm_attrs.data());
    }

    /* Flush the bulker to perform the sync. */
    nextHopGroupMemberBulker.flush();

    /*
     * Go through the synced members and increment the Crm ref count for the
     * successful ones.
     */
    for (const auto& mbr : syncingMembers)
    {
        /* Check that the returned member ID is valid. */
        if (mbr.second == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Failed to create next hop group %s's member %s",
                            m_key.to_string().c_str(), mbr.first.to_string().c_str());
            success = false;
        }
        else
        {
            m_members.at(mbr.first).sync(mbr.second);
        }
    }

    return success;
}

/*
 * Purpose:     Update the next hop group based on a new next hop group key.
 * Description: Update the group's members by removing the members that aren't
 *              in the new next hop group and adding the new members.  We first
 *              remove the missing members to avoid cases where we reached the
 *              ASIC group members limit.  This will not update the group's SAI
 *              ID in any way, unless we are promoting a temporary group.
 * Params:      IN  nhg_key - The new next hop group key to update to.
 * Returns:     true, if the operation was successful;
 *              false, otherwise.
 */
bool NextHopGroup::update(const NextHopGroupKey& nhg_key)
{
    SWSS_LOG_ENTER();

    if (!isSynced() ||
        (!isRecursive() && (m_members.size() == 1 || nhg_key.getSize() == 1)))
    {
        bool was_synced = isSynced();
        bool was_temp = isTemp();
        *this = NextHopGroup(nhg_key, false);

        /*
        * For temporary nexthop group being updated, set the recursive flag
        * as it is expected to get promoted to multiple NHG
        */
        setRecursive(was_temp);

        /* Sync the group only if it was synced before. */
        return (was_synced ? sync() : true);
    }

    /* Update the key. */
    m_key = nhg_key;

    std::set<NextHopKey> new_nh_keys = nhg_key.getNextHops();
    std::set<NextHopKey> removed_nh_keys;

    /* Mark the members that need to be removed. */
    for (auto& mbr_it : m_members)
    {
        const NextHopKey& nh_key = mbr_it.first;

        /* Look for the existing member inside the new ones. */
        const auto& new_nh_key_it = new_nh_keys.find(nh_key);

        /* If the member is not found, then it needs to be removed. */
        if (new_nh_key_it == new_nh_keys.end())
        {
            removed_nh_keys.insert(nh_key);
        }
        /* If the member is updated, update it's weight. */
        else
        {
            if (new_nh_key_it->weight && mbr_it.second.getWeight() != new_nh_key_it->weight && !mbr_it.second.updateWeight(new_nh_key_it->weight))
            {
                SWSS_LOG_WARN("Failed to update member %s weight", nh_key.to_string().c_str());
                return false;
            }

            /*
             * Erase the member from the new members list as it already
             * exists.
             */
            new_nh_keys.erase(new_nh_key_it);
        }
    }

    /* Remove the removed members. */
    if (!removeMembers(removed_nh_keys))
    {
        SWSS_LOG_WARN("Failed to remove members from group %s", to_string().c_str());
        return false;
    }

    /* Remove the removed members. */
    for (const auto& nh_key : removed_nh_keys)
    {
        m_members.erase(nh_key);
    }

    /* Add any new members to the group. */
    for (const auto& it : new_nh_keys)
    {
        m_members.emplace(it, NextHopGroupMember(it));
    }

    /*
     * Sync all the members of the group.  We sync all of them because
     * there may be previous members that were not successfully synced
     * before the update, so we must make sure we sync those as well.
     */
    if (!syncMembers(m_key.getNextHops()))
    {
        SWSS_LOG_WARN("Failed to sync new members for group %s", to_string().c_str());
        return false;
    }

    return true;
}

/*
 * Purpose:     Create the attributes vector for a next hop group member.
 * Description: Create the group ID and next hop ID attributes.
 * Params:      IN  nhgm - The next hop group member.
 * Returns:     The attributes vector for the given next hop.
 */
vector<sai_attribute_t> NextHopGroup::createNhgmAttrs(const NextHopGroupMember& nhgm) const
{
    SWSS_LOG_ENTER();

    vector<sai_attribute_t> nhgm_attrs;
    sai_attribute_t nhgm_attr;

    /* Fill in the group ID. */
    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
    nhgm_attr.value.oid = m_id;
    nhgm_attrs.push_back(nhgm_attr);

    /* Fill in the next hop ID. */
    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    nhgm_attr.value.oid = nhgm.getNhId();
    nhgm_attrs.push_back(nhgm_attr);

    /* Fill in the weight if set. */
    auto weight  = nhgm.getWeight();
    if (weight != 0)
    {
        nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_WEIGHT;
        nhgm_attr.value.s32 = weight;
        nhgm_attrs.push_back(nhgm_attr);
    }

    return nhgm_attrs;
}

/*
 * Purpose:     Validate a next hop in the group.
 * Description: Sync the validated next hop group member.
 * Params:      IN  nh_key - The next hop to validate.
 * Returns:     true, if the operation was successful;
 *              false, otherwise.
 */
bool NextHopGroup::validateNextHop(const NextHopKey& nh_key)
{
    SWSS_LOG_ENTER();

    if (isRecursive() || (m_members.size() > 1))
    {
        return syncMembers({nh_key});
    }

    return true;
}

/*
 * Purpose:     Invalidate a next hop in the group.
 * Description: Sync the invalidated next hop group member.
 * Params:      IN  nh_key - The next hop to invalidate.
 * Returns:     true, if the operation was successful;
 *              false, otherwise.
 */
bool NextHopGroup::invalidateNextHop(const NextHopKey& nh_key)
{
    SWSS_LOG_ENTER();

    if (isRecursive() || (m_members.size() > 1))
    {
        return removeMembers({nh_key});
    }

    return true;
}
