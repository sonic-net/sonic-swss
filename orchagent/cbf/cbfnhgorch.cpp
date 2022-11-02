#include "cbfnhgorch.h"
#include "crmorch.h"
#include "bulker.h"
#include "tokenize.h"
#include "nhgorch.h"
#include "nhgmaporch.h"
#include "routeorch.h"

extern sai_object_id_t gSwitchId;

extern NhgOrch *gNhgOrch;
extern CbfNhgOrch *gCbfNhgOrch;
extern CrmOrch *gCrmOrch;
extern NhgMapOrch *gNhgMapOrch;
extern RouteOrch *gRouteOrch;

extern sai_next_hop_group_api_t* sai_next_hop_group_api;

extern size_t gMaxBulkSize;

CbfNhgOrch::CbfNhgOrch(DBConnector *db, string tableName) : NhgOrchCommon(db, tableName)
{
    SWSS_LOG_ENTER();
}

/*
 * Purpose:     Perform the operations requested by APPL_DB users.
 *
 * Description: Iterate over the untreated operations list and resolve them.
 *              The operations supported are SET and DEL.  If an operation
 *              could not be resolved, it will either remain in the list, or be
 *              removed, depending on the case.
 *
 * Params:      IN  consumer - The cosumer object.
 *
 * Returns:     Nothing.
 */
void CbfNhgOrch::doTask(Consumer& consumer)
{
    SWSS_LOG_ENTER();

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();

    while (it != consumer.m_toSync.end())
    {
        swss::KeyOpFieldsValuesTuple t = it->second;

        string index = kfvKey(t);
        string op = kfvOp(t);

        bool success;
        const auto &cbf_nhg_it = m_syncdNextHopGroups.find(index);

        if (op == SET_COMMAND)
        {
            string members;
            string selection_map;

            /*
             * Get CBF group's members and selection map.
             */
            for (const auto &i : kfvFieldsValues(t))
            {
                if (fvField(i) == "members")
                {
                    members = fvValue(i);
                }
                else if (fvField(i) == "selection_map")
                {
                    selection_map = fvValue(i);
                }
            }

            /*
             * Validate the data.
             */
            auto p = getMembers(members);

            if (!p.first)
            {
                SWSS_LOG_ERROR("CBF next hop group %s data is invalid.",
                               index.c_str());
                it = consumer.m_toSync.erase(it);
                continue;
            }

            /*
             * If the CBF group does not exist, create it.
             */
            if (cbf_nhg_it == m_syncdNextHopGroups.end())
            {
                /*
                 * If we reached the NHG limit, postpone the creation.
                 */
                if (gRouteOrch->getNhgCount() + NhgBase::getSyncedCount() >= gRouteOrch->getMaxNhgCount())
                {
                    SWSS_LOG_WARN("Reached next hop group limit. Postponing creation.");
                    success = false;
                }
                else
                {
                    auto cbf_nhg = std::make_unique<CbfNhg>(index, p.second, selection_map);
                    success = cbf_nhg->sync();

                    if (success)
                    {
                        /*
                         * If the CBF NHG contains temporary NHGs as members,
                         * we have to keep checking for updates.
                         */
                        if (cbf_nhg->hasTemps())
                        {
                            success = false;
                        }

                        m_syncdNextHopGroups.emplace(index, NhgEntry<CbfNhg>(move(cbf_nhg)));
                    }
                }
            }
            /*
             * If the CBF group exists, update it.
             */
            else
            {
                success = cbf_nhg_it->second.nhg->update(p.second, selection_map);

                /*
                 * If the CBF NHG has temporary NHGs synced, we need to keep
                 * checking this group in case they are promoted.
                 */
                if (cbf_nhg_it->second.nhg->hasTemps())
                {
                    success = false;
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
            /* If the group doesn't exist, do nothing. */
            else if (cbf_nhg_it == m_syncdNextHopGroups.end())
            {
                SWSS_LOG_WARN("Deleting inexistent CBF NHG %s", index.c_str());
                /*
                 * Mark it as a success to remove the task from the consumer.
                 */
                success = true;
            }
            /* If the group does exist but is still referenced, skip.*/
            else if (cbf_nhg_it->second.ref_count > 0)
            {
                SWSS_LOG_WARN("Skipping removal of CBF next hop group %s which"
                              " is still referenced", index.c_str());
                success = false;
            }
            /* Otherwise, delete it. */
            else
            {
                success = cbf_nhg_it->second.nhg->remove();

                if (success)
                {
                    m_syncdNextHopGroups.erase(cbf_nhg_it);
                }
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown operation type %s", op.c_str());
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
 * Purpose: Validate the CBF members.
 *
 * Params:  IN members - The members string to validate.
 *
 * Returns: Pair where:
 *          - the first element is a bool, representing if the members are
 *            valid or not
 *          - the second element is a vector of members
 */
pair<bool, vector<string>> CbfNhgOrch::getMembers(const string &members)
{
    SWSS_LOG_ENTER();

    auto members_vec = swss::tokenize(members, ',');
    set<string> members_set(members_vec.begin(), members_vec.end());
    bool success = true;

    /*
     * Verify that the members list is not empty
     */
    if (members_set.empty())
    {
        SWSS_LOG_ERROR("CBF next hop group members list is empty.");
        success = false;
    }
    /*
     * Verify that the members are unique.
     */
    else if (members_set.size() != members_vec.size())
    {
        SWSS_LOG_ERROR("CBF next hop group members are not unique.");
        success = false;
    }

    return {success, members_vec};
}

/*
 * Purpose: Constructor.
 *
 * Params:  IN index - The index of the CBF NHG.
 *          IN members - The members of the CBF NHG.
 *          IN selection_map - The selection map index of the CBF NHG.
 *
 * Returns: Nothing.
 */
CbfNhg::CbfNhg(const string &index,
               const vector<string> &members,
               const string &selection_map) :
    NhgCommon(index),
    m_selection_map(selection_map)
{
    SWSS_LOG_ENTER();

    uint8_t idx = 0;
    for (const auto &member : members)
    {
        m_members.emplace(member, CbfNhgMember(member, idx++));
    }
}

/*
 * Purpose: Move constructor.
 *
 * Params:  IN cbf_nhg - The temporary object to construct from.
 *
 * Returns: Nothing.
 */
CbfNhg::CbfNhg(CbfNhg &&cbf_nhg) :
    NhgCommon(move(cbf_nhg)),
    m_selection_map(move(cbf_nhg.m_selection_map)),
    m_temp_nhgs(move(cbf_nhg.m_temp_nhgs))
{
    SWSS_LOG_ENTER();
}

/*
 * Purpose: Sync the CBF NHG over SAI, getting a SAI ID.
 *
 * Params:  None.
 *
 * Returns: true, if the operation was successful,
 *          false, otherwise.
 */
bool CbfNhg::sync()
{
    SWSS_LOG_ENTER();

    /* If the group is already synced, exit. */
    if (isSynced())
    {
        return true;
    }

    /* Create the CBF next hop group over SAI. */
    sai_attribute_t nhg_attr;
    vector<sai_attribute_t> nhg_attrs;

    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
    nhg_attr.value.u32 = SAI_NEXT_HOP_GROUP_TYPE_CLASS_BASED;
    nhg_attrs.push_back(move(nhg_attr));

    /* Add the number of members. */
    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_CONFIGURED_SIZE;
    assert(m_members.size() <= UINT32_MAX);
    nhg_attr.value.u32 = static_cast<sai_uint32_t>(m_members.size());
    nhg_attrs.push_back(move(nhg_attr));

    if (nhg_attr.value.u32 > gNhgMapOrch->getMaxNumFcs())
    {
        /* If there are more members than FCs then this may be an error, as some members won't be used. */
        SWSS_LOG_WARN("More CBF NHG members configured than supported Forwarding Classes");
    }

    /* Add the selection map to the attributes. */
    nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_SELECTION_MAP;
    nhg_attr.value.oid = gNhgMapOrch->getMapId(m_selection_map);

    if (nhg_attr.value.oid == SAI_NULL_OBJECT_ID)
    {
        SWSS_LOG_ERROR("FC to NHG map index %s does not exist", m_selection_map.c_str());
        return false;
    }

    if ((unsigned int)gNhgMapOrch->getLargestNhIndex(m_selection_map) >= m_members.size())
    {
        SWSS_LOG_ERROR("FC to NHG map references more NHG members than exist in group %s", m_key.c_str());
        return false;
    }

    nhg_attrs.push_back(move(nhg_attr));

    auto status = sai_next_hop_group_api->create_next_hop_group(
                                    &m_id,
                                    gSwitchId,
                                    static_cast<uint32_t>(nhg_attrs.size()),
                                    nhg_attrs.data());

    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to create CBF next hop group %s, rv %d",
                        m_key.c_str(),
                        status);
        task_process_status handle_status = gCbfNhgOrch->handleSaiCreateStatus(SAI_API_NEXT_HOP_GROUP, status);
        if (handle_status != task_success)
        {
            return gCbfNhgOrch->parseHandleSaiStatusFailure(handle_status);
        }
    }

    /* Increase the reference counter for the selection map. */
    gNhgMapOrch->incRefCount(m_selection_map);

    /*
     * Increment the amount of programmed next hop groups. */
    gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    incSyncedCount();

    /* Sync the group members. */
    set<string> members;

    for (const auto &member : m_members)
    {
        members.insert(member.first);
    }

    if (!syncMembers(members))
    {
        SWSS_LOG_ERROR("Failed to sync CBF next hop group %s", m_key.c_str());
        return false;
    }

    return true;
}

/*
 * Purpose: Remove the CBF NHG over SAI, removing the SAI ID.
 *
 * Params:  None.
 *
 * Returns: true, if the operation was successful,
 *          false, otherwise.
 */
bool CbfNhg::remove()
{
    SWSS_LOG_ENTER();

    bool is_synced = isSynced();

    bool success = NhgCommon::remove();

    if (success && is_synced)
    {
        gNhgMapOrch->decRefCount(m_selection_map);
    }

    return success;
}

/*
 * Purpose: Check if the CBF NHG has the same members and in the same order as
 *          the ones given.
 *
 * Params:  IN members - The members to compare with.
 *
 * Returns: true, if the current members are the same with the given one,
 *          false, otherwise.
 */
bool CbfNhg::hasSameMembers(const vector<string> &members) const
{
    SWSS_LOG_ENTER();

    /* The size should be the same. */
    if (m_members.size() != members.size())
    {
        return false;
    }

    /*
     * Check that the members are the same and the index is preserved.
     */
    uint8_t index = 0;

    for (const auto &member : members)
    {
        auto mbr_it = m_members.find(member);

        if (mbr_it == m_members.end())
        {
            return false;
        }

        if (mbr_it->second.getIndex() != index++)
        {
            return false;
        }
    }

    return true;
}

/*
 * Purpose: Update a CBF next hop group.
 *
 * Params:  IN members - The new members.
 *          IN selection_map - The new selection map.
 *
 * Returns: true, if the update was successful,
 *          false, otherwise.
 */
bool CbfNhg::update(const vector<string> &members, const string &selection_map)
{
    SWSS_LOG_ENTER();

    /*
     * Check if we're just checking if the temporary NHG members were updated.
     * This would happen only if the given members are the same with the
     * existing ones and in the same order.  In this scenario, we'll update
     * just the NEXT_HOP attribute of the temporary members if necessary.
     */
    if (!m_temp_nhgs.empty() && hasSameMembers(members))
    {
        /* Iterate over the temporary NHGs and check if they were updated. */
        for (auto member = m_temp_nhgs.begin(); member != m_temp_nhgs.end();)
        {
            const auto &nhg = gNhgOrch->getNhg(member->first);

            /*
             * If the NHG ID has changed since storing the first occurence,
             * we have to update the CBF NHG member's attribute.
             */
            if (nhg.getId() != member->second)
            {
                if (!m_members.at(member->first).updateNhAttr())
                {
                    SWSS_LOG_ERROR("Failed to update temporary next hop group "
                                    "member %s of CBF next hop group %s",
                                    member->first.c_str(), m_key.c_str());
                    return false;
                }

                /* If the NHG was promoted, remove it from the temporary NHG map. */
                if (!nhg.isTemp())
                {
                    member = m_temp_nhgs.erase(member);
                }
                /*
                 * If the NHG was just updated, update its current NHG ID value
                 * in the map.
                 */
                else
                {
                    member->second = nhg.getId();
                    ++member;
                }
            }
            else
            {
                ++member;
            }
        }
    }
    /* If the members are different, update the whole members list. */
    else
    {
        /*
         * Because the INDEX attribute is CREATE_ONLY, we have to remove all
         * the existing members and sync the new ones back as the order of the
         * members can change due to some of them being removed, which would
         * invalidate all the other members following them, or inserting a new
         * one somewhere in the front of the existing members which would also
         * invalidate them.
         */
        set<string> members_set;

        for (const auto &member : m_members)
        {
            members_set.insert(member.first);
        }

        /* Remove the existing members. */
        if (!removeMembers(members_set))
        {
            SWSS_LOG_ERROR("Failed to remove members of CBF next hop group %s",
                            m_key.c_str());
            return false;
        }
        m_members.clear();
        m_temp_nhgs.clear();

        /* Add the new members. */
        uint8_t index = 0;
        for (const auto &member : members)
        {
            m_members.emplace(member, CbfNhgMember(member, index++));
        }

        if ((unsigned int)gNhgMapOrch->getLargestNhIndex(m_selection_map) >= m_members.size())
        {
            SWSS_LOG_ERROR("FC to NHG map references more NHG members than exist in group %s",
                           m_key.c_str());
            return false;
        }

        /* Sync the new members. */
        if (!syncMembers({members.begin(), members.end()}))
        {
            SWSS_LOG_ERROR("Failed to sync members of CBF next hop group %s",
                            m_key.c_str());
            return false;
        }
    }

    /* Update the group map. */
    if (m_selection_map != selection_map)
    {
        sai_attribute_t nhg_attr;
        nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_SELECTION_MAP;
        nhg_attr.value.oid = gNhgMapOrch->getMapId(selection_map);

        if (nhg_attr.value.oid == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("NHG map %s does not exist", selection_map.c_str());
            return false;
        }

        if ((unsigned int)gNhgMapOrch->getLargestNhIndex(selection_map) >= m_members.size())
        {
            SWSS_LOG_ERROR("FC to NHG map references more NHG members than exist in group %s",
                           m_key.c_str());
            return false;
        }

        auto status = sai_next_hop_group_api->set_next_hop_group_attribute(m_id, &nhg_attr);

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to update CBF next hop group %s, rv %d",
                            m_key.c_str(),
                            status);
            return false;
        }

        /* Update the selection map and update the previous and new map ref count. */
        gNhgMapOrch->decRefCount(m_selection_map);
        m_selection_map = selection_map;
        gNhgMapOrch->incRefCount(m_selection_map);
    }

    return true;
}

/*
 * Purpose: Sync the given CBF group members.
 *
 * Params:  IN members - The members to sync.
 *
 * Returns: true, if the operation was successful,
 *          false, otherwise.
 */
bool CbfNhg::syncMembers(const set<string> &members)
{
    SWSS_LOG_ENTER();

    /* The group should be synced at this point. */
    if (!isSynced())
    {
        SWSS_LOG_ERROR("Trying to sync members of CBF next hop group %s which is not synced",
                       m_key.c_str());
        throw logic_error("Syncing members of unsynced CBF next hop group");
    }

    /*
     * Sync all the given members.  If a NHG does not exist, is not yet synced
     * or is temporary, stop immediately.
     */
    ObjectBulker<sai_next_hop_group_api_t> bulker(sai_next_hop_group_api,
                                                  gSwitchId,
                                                  gMaxBulkSize);
    unordered_map<string, sai_object_id_t> nhgm_ids;

    for (const auto &key : members)
    {
        auto &nhgm = m_members.at(key);

        /* A next hop group member can't be already synced if it was passed into the syncMembers() method. */
        if (nhgm.isSynced())
        {
            SWSS_LOG_ERROR("Trying to sync already synced CBF NHG member %s in group %s",
                           nhgm.to_string().c_str(), to_string().c_str());
            throw std::logic_error("Syncing already synced NHG member");
        }
        else if (nhgm.getNhgId() == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_WARN("CBF NHG member %s is not yet synced", nhgm.to_string().c_str());
            return false;
        }

        /*
         * Check if the group exists in NhgOrch.
         */
        if (!gNhgOrch->hasNhg(key))
        {
            SWSS_LOG_ERROR("Next hop group %s in CBF next hop group %s does "
                            "not exist", key.c_str(), m_key.c_str());
            return false;
        }

        const auto &nhg = gNhgOrch->getNhg(key);

        /*
         * Check if the group is synced.
         */
        if (!nhg.isSynced())
        {
            SWSS_LOG_ERROR("Next hop group %s in CBF next hop group %s is not"
                            " synced",
                            key.c_str(), m_key.c_str());
            return false;
        }

        /* Create the SAI attributes for syncing the NHG as a member. */
        auto attrs = createNhgmAttrs(nhgm);

        bulker.create_entry(&nhgm_ids[key],
                            (uint32_t)attrs.size(),
                            attrs.data());
    }

    /* Flush the bulker to perform the sync. */
    bulker.flush();

    /* Iterate over the synced members and set their SAI ID. */
    bool success = true;

    for (const auto &member : nhgm_ids)
    {
        if (member.second == SAI_NULL_OBJECT_ID)
        {
            SWSS_LOG_ERROR("Failed to create CBF next hop group %s member %s",
                            m_key.c_str(), member.first.c_str());
            success = false;
        }
        else
        {
            m_members.at(member.first).sync(member.second);

            const auto &nhg = gNhgOrch->getNhg(member.first);
            /*
             * If the member is temporary, store it in order to check if it is
             * promoted at some point.
             */
            if (nhg.isTemp())
            {
                m_temp_nhgs.emplace(member.first, nhg.getId());
            }
        }
    }

    return success;
}

/*
 * Purpose: Create a vector with the SAI attributes for syncing a next hop
 *          group member over SAI.  The caller is reponsible of filling in the
 *          index attribute.
 *
 * Params:  IN nhgm - The next hop group member to sync.
 *
 * Returns: The vector containing the SAI attributes.
 */
vector<sai_attribute_t> CbfNhg::createNhgmAttrs(const CbfNhgMember &nhgm) const
{
    SWSS_LOG_ENTER();

    if (!isSynced())
    {
        SWSS_LOG_ERROR("CBF next hop group %s is not synced", to_string().c_str());
        throw logic_error("CBF next hop group member attributes data is insufficient");
    }

    vector<sai_attribute_t> attrs;
    sai_attribute_t attr;

    /* Fill in the group ID. */
    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
    attr.value.oid = m_id;
    attrs.push_back(attr);

    /* Fill in the next hop ID. */
    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    attr.value.oid = nhgm.getNhgId();
    attrs.push_back(attr);

    /* Fill in the index. */
    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_INDEX;
    attr.value.oid = nhgm.getIndex();
    attrs.push_back(attr);

    return attrs;
}

/*
 * Purpose: Sync the member, setting its SAI ID and incrementing the necessary
 *          ref counters.
 *
 * Params:  IN gm_id - The SAI ID to set.
 *
 * Returns: Nothing.
 */
void CbfNhgMember::sync(sai_object_id_t gm_id)
{
    SWSS_LOG_ENTER();

    NhgMember::sync(gm_id);
    gNhgOrch->incNhgRefCount(m_key);
}

/*
 * Purpose: Update the next hop attribute of the member.
 *
 * Params:  None.
 *
 * Returns: true, if the operation was successful,
 *          false, otherwise.
 */
bool CbfNhgMember::updateNhAttr()
{
    SWSS_LOG_ENTER();

    if (!isSynced())
    {
        SWSS_LOG_ERROR("Trying to update next hop attribute of CBF NHG member %s that is not synced",
                        to_string().c_str());
        throw logic_error("Trying to update attribute of unsynced object.");
    }

    /*
     * Fill in the attribute.
     */
    sai_attribute_t attr;
    attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    attr.value.oid = getNhgId();

    /*
     * Set the attribute over SAI.
     */
    auto status = sai_next_hop_group_api->set_next_hop_group_member_attribute(m_gm_id, &attr);

    return status == SAI_STATUS_SUCCESS;
}

/*
 * Purpose: Remove the member, reseting its SAI ID and decrementing the NHG ref
 *          counter.
 *
 * Params:  None.
 *
 * Returns: Nothing.
 */
void CbfNhgMember::remove()
{
    SWSS_LOG_ENTER();

    NhgMember::remove();
    gNhgOrch->decNhgRefCount(m_key);
}

/*
 * Purpose: Get the NHG ID of this member.
 *
 * Params:  None.
 *
 * Returns: The SAI ID of the NHG it references or SAI_NULL_OBJECT_ID if it
 *          doesn't exist.
 */
sai_object_id_t CbfNhgMember::getNhgId() const
{
    SWSS_LOG_ENTER();

    if (!gNhgOrch->hasNhg(m_key))
    {
        return SAI_NULL_OBJECT_ID;
    }

    return gNhgOrch->getNhg(m_key).getId();
}
