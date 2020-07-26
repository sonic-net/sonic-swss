#include <assert.h>
#include <inttypes.h>
#include "fgnhgorch.h"
#include "routeorch.h"
#include "logger.h"
#include "swssnet.h"
#include "crmorch.h"
#include <array>

extern sai_object_id_t gVirtualRouterId;
extern sai_object_id_t gSwitchId;

extern sai_next_hop_group_api_t*    sai_next_hop_group_api;
extern sai_route_api_t*             sai_route_api;

extern RouteOrch *gRouteOrch;
extern CrmOrch *gCrmOrch;
int fgnhgorch_pri = 5;


FgNhgOrch::FgNhgOrch(DBConnector *db, DBConnector *stateDb, vector<string> &tableNames, NeighOrch *neighOrch, IntfsOrch *intfsOrch, VRFOrch *vrfOrch) :
        Orch(db, tableNames),
        m_neighOrch(neighOrch),
        m_intfsOrch(intfsOrch),
        m_vrfOrch(vrfOrch),
		m_stateWarmRestartRouteTable(stateDb, STATE_FG_ROUTE_TABLE_NAME)
{
     SWSS_LOG_ENTER();
}

void calculate_bank_hash_bucket_start_indices(FgNhgEntry *fgNhgEntry)
{
    SWSS_LOG_ENTER();
    uint32_t num_banks = 0;
    vector<uint32_t> memb_per_bank;
    for(auto nh : fgNhgEntry->nextHops)
    {
        if(nh.second + 1 > num_banks)
        {
            num_banks = nh.second + 1;
            memb_per_bank.push_back(0);
        }
        memb_per_bank[nh.second] = memb_per_bank[nh.second] + 1;
    }

    uint32_t buckets_per_nexthop = fgNhgEntry->real_bucket_size/((uint32_t)fgNhgEntry->nextHops.size());
    uint32_t extra_buckets = fgNhgEntry->real_bucket_size - (buckets_per_nexthop*((uint32_t)fgNhgEntry->nextHops.size()));
    uint32_t split_extra_buckets_among_bank = extra_buckets/num_banks;
    extra_buckets = extra_buckets - (split_extra_buckets_among_bank*num_banks);

    uint32_t prev_idx = 0;

    for(uint32_t i = 0; i < memb_per_bank.size(); i++)
    {
        bank_index_range bir;
        bir.start_index = prev_idx;
        bir.end_index = bir.start_index + (buckets_per_nexthop * memb_per_bank[i]) + split_extra_buckets_among_bank - 1;
        if(extra_buckets > 0)
        {
            bir.end_index = bir.end_index + 1;
            extra_buckets--;
        }
        if(i == fgNhgEntry->hash_bucket_indices.size())
        {
            fgNhgEntry->hash_bucket_indices.push_back(bir);
        }
        else
        {
            fgNhgEntry->hash_bucket_indices[i] = bir;
        }
        prev_idx = bir.end_index + 1;
        SWSS_LOG_INFO("Calculate_bank_hash_bucket_start_indices: bank %d, si %d, ei %d",
                       i, fgNhgEntry->hash_bucket_indices[i].start_index, fgNhgEntry->hash_bucket_indices[i].end_index);
    }
}


void FgNhgOrch::set_state_db_route_entry(const IpPrefix &ipPrefix, uint32_t index, NextHopKey nextHop)
{
    SWSS_LOG_ENTER();

    string key = ipPrefix.to_string();
    // Write to StateDb
    std::vector<FieldValueTuple> fvs;

    // check if profile already exists - if yes - skip creation
    m_stateWarmRestartRouteTable.get(key, fvs);

    //bucket rewrite
    if (fvs.size() > index)
    {
        FieldValueTuple fv(std::to_string(index), nextHop.to_string());
        fvs[index] = fv;
        SWSS_LOG_INFO("Set state db entry for ip prefix %s next hop %s with index %d",
                        ipPrefix.to_string().c_str(), nextHop.to_string().c_str(), index);
        m_stateWarmRestartRouteTable.set(key, fvs);
    }
    else
    {
        fvs.push_back(FieldValueTuple(std::to_string(index), nextHop.to_string()));
        SWSS_LOG_INFO("Add new next hop entry %s with index %d for ip prefix %s",
                nextHop.to_string().c_str(), index, ipPrefix.to_string().c_str());
        m_stateWarmRestartRouteTable.set(key, fvs);
    }

}


bool FgNhgOrch::write_hash_bucket_change_to_sai(FGNextHopGroupEntry *syncd_fg_route_entry, uint32_t index, sai_object_id_t nh_oid,
        const IpPrefix &ipPrefix, NextHopKey nextHop)
{
    SWSS_LOG_ENTER();

    sai_attribute_t nhgm_attr;
    nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
    nhgm_attr.value.oid = nh_oid;
    sai_status_t status = sai_next_hop_group_api->set_next_hop_group_member_attribute(
                                                              syncd_fg_route_entry->nhopgroup_members[index],
                                                              &nhgm_attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to set next hop oid %" PRIx64 " member %" PRIx64 ": %d\n",
            syncd_fg_route_entry->nhopgroup_members[index], nh_oid, status);
        return false;
    }

    set_state_db_route_entry(ipPrefix, index, nextHop);
    return true;
}


bool FgNhgOrch::validnexthopinNextHopGroup(const NextHopKey& nexthop)
{
    SWSS_LOG_ENTER();

    for( auto &route_tables : m_syncdFGRouteTables )
    {
        for( auto &route_table : route_tables.second )
        {
            if(!(route_table.second.nhg_key.contains(nexthop)))
            {
                continue;
            }

            FGNextHopGroupEntry *syncd_fg_route_entry = &(route_table.second);
            auto prefix_entry = fgNhgPrefixes.find(route_table.first);
            assert(prefix_entry != fgNhgPrefixes.end());
            FgNhgEntry *fgNhgEntry = prefix_entry->second;
            std::map<NextHopKey,sai_object_id_t> nhopgroup_members_set;

            std::vector<Bank_Member_Changes> bank_member_changes(
                fgNhgEntry->hash_bucket_indices.size(), Bank_Member_Changes());

            if(syncd_fg_route_entry->active_nexthops.find(nexthop) !=
                    syncd_fg_route_entry->active_nexthops.end())
            {
                return true;
            }

            for( auto active_nh : syncd_fg_route_entry->active_nexthops)
            {
                bank_member_changes[fgNhgEntry->nextHops[active_nh.ip_address]].
                    active_nhs.push_back(active_nh);
            }

            bank_member_changes[fgNhgEntry->nextHops[nexthop.ip_address]].
                    nhs_to_add.push_back(nexthop);
            nhopgroup_members_set[nexthop] = m_neighOrch->getNextHopId(nexthop);

            if(!compute_and_set_hash_bucket_changes(syncd_fg_route_entry, fgNhgEntry, 
                    bank_member_changes, nhopgroup_members_set, route_table.first))
            {
                SWSS_LOG_ERROR("Failed to set fine grained next hop %s",
                    nexthop.to_string().c_str());
                return false;
            }

            m_neighOrch->increaseNextHopRefCount(nexthop);
        }
    }

    return true;
}


bool FgNhgOrch::invalidnexthopinNextHopGroup(const NextHopKey& nexthop)
{
    SWSS_LOG_ENTER();

    for( auto &route_tables : m_syncdFGRouteTables )
    {
        for( auto &route_table : route_tables.second )
        {
            if(!(route_table.second.nhg_key.contains(nexthop)))
            {
                continue;
            }

            FGNextHopGroupEntry *syncd_fg_route_entry = &(route_table.second);
            auto prefix_entry = fgNhgPrefixes.find(route_table.first);
            assert(prefix_entry != fgNhgPrefixes.end());
            FgNhgEntry *fgNhgEntry = prefix_entry->second;
            std::map<NextHopKey,sai_object_id_t> nhopgroup_members_set;

            std::vector<Bank_Member_Changes> bank_member_changes(
                fgNhgEntry->hash_bucket_indices.size(), Bank_Member_Changes());

            if(syncd_fg_route_entry->active_nexthops.find(nexthop) ==
                    syncd_fg_route_entry->active_nexthops.end())
            {
                return true;
            }

            for( auto active_nh : syncd_fg_route_entry->active_nexthops)
            {
                if(active_nh.ip_address == nexthop.ip_address &&
                        active_nh.alias == nexthop.alias)
                {
                    continue;
                }

                bank_member_changes[fgNhgEntry->nextHops[active_nh.ip_address]].
                    active_nhs.push_back(active_nh);

                nhopgroup_members_set[active_nh] = m_neighOrch->getNextHopId(active_nh);
            }

            bank_member_changes[fgNhgEntry->nextHops[nexthop.ip_address]].
                    nhs_to_del.push_back(nexthop);

            if(!compute_and_set_hash_bucket_changes(syncd_fg_route_entry, fgNhgEntry, 
                    bank_member_changes, nhopgroup_members_set, route_table.first))
            {
                SWSS_LOG_ERROR("Failed to set fine grained next hop %s",
                    nexthop.to_string().c_str());
                return false;
            }

            m_neighOrch->decreaseNextHopRefCount(nexthop);
        }
    }

    return true;
}


bool FgNhgOrch::remove_nhg(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry)
{
    SWSS_LOG_ENTER();
    sai_status_t status;

    for(auto nhgm : syncd_fg_route_entry->nhopgroup_members)
    {
        status = sai_next_hop_group_api->remove_next_hop_group_member(nhgm);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to remove next hop group member %" PRIx64 ", rv:%d",
                nhgm, status);
            return false;
        }
        gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
    }

    status = sai_next_hop_group_api->remove_next_hop_group(syncd_fg_route_entry->next_hop_group_id);
    if (status != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_ERROR("Failed to remove next hop group %" PRIx64 ", rv:%d",
                syncd_fg_route_entry->next_hop_group_id, status);
        return false;
    }
    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);
    gRouteOrch->decNextHopGroupCount();

    return true;
}


bool FgNhgOrch::set_active_bank_hash_bucket_changes(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        uint32_t bank, uint32_t syncd_bank, std::vector<Bank_Member_Changes> bank_member_changes, 
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    Bank_Member_Changes bank_member_change = bank_member_changes[bank];
    uint32_t add_idx = 0, del_idx = 0;
    FGNextHopGroupMap *bank_fgnhg_map = &(syncd_fg_route_entry->syncd_fgnhg_map[syncd_bank]);

    while(del_idx < bank_member_change.nhs_to_del.size() &&
            add_idx < bank_member_change.nhs_to_add.size())
    {
        HashBuckets *hash_buckets = &(bank_fgnhg_map->at(bank_member_change.nhs_to_del[del_idx]));
        for(uint32_t i = 0; i < hash_buckets->size(); i++)
        {
            if(!write_hash_bucket_change_to_sai(syncd_fg_route_entry, hash_buckets->at(i), 
                        nhopgroup_members_set[bank_member_change.nhs_to_add[add_idx]],
                        ipPrefix, bank_member_change.nhs_to_add[add_idx]))
            {
                return false;
            }
        }

        (*bank_fgnhg_map)[bank_member_change.nhs_to_add[add_idx]] =*hash_buckets;

        bank_fgnhg_map->erase(bank_member_change.nhs_to_del[del_idx]);
        bank_member_change.active_nhs.push_back(bank_member_change.nhs_to_add[add_idx]);
        syncd_fg_route_entry->active_nexthops.erase(bank_member_change.nhs_to_del[del_idx]);
        syncd_fg_route_entry->active_nexthops.insert(bank_member_change.nhs_to_add[add_idx]);
        del_idx++;
        add_idx++;
    }

    /* Given that we resolved add + del on a bank in the above while stmt
     * We will either have add OR delete left to do, and the logic below 
     * relies on this fact
     */
    if(del_idx < bank_member_change.nhs_to_del.size())
    {
        for(auto memb: bank_member_change.nhs_to_add)
        {
            /* Create collated list of members */
            bank_member_change.active_nhs.push_back(memb);
        }
        while(del_idx < bank_member_change.nhs_to_del.size())
        {
            HashBuckets *hash_buckets = &(bank_fgnhg_map->at(bank_member_change.nhs_to_del[del_idx]));
            for(uint32_t i = 0; i < hash_buckets->size(); i++)
            {
                NextHopKey round_robin_nh = bank_member_change.active_nhs[i %
                    bank_member_change.active_nhs.size()];

                if(!write_hash_bucket_change_to_sai(syncd_fg_route_entry, hash_buckets->at(i), 
                        nhopgroup_members_set[round_robin_nh], ipPrefix, round_robin_nh))
                {
                    return false;
                }
                bank_fgnhg_map->at(round_robin_nh).push_back(hash_buckets->at(i));
                /* TODO: simple round robin can make the hash non-balanced, rebalance the hash */
            }
            bank_fgnhg_map->erase(bank_member_change.nhs_to_del[del_idx]);
            syncd_fg_route_entry->active_nexthops.erase(bank_member_change.nhs_to_del[del_idx]);
            del_idx++;
        }
    }

    if(add_idx < bank_member_change.nhs_to_add.size())
    {
        uint32_t exp_bucket_size = (1+ fgNhgEntry->hash_bucket_indices[syncd_bank].end_index - 
            fgNhgEntry->hash_bucket_indices[syncd_bank].start_index)/
            ((uint32_t)bank_member_change.active_nhs.size() + 
             (uint32_t)bank_member_change.nhs_to_add.size());

        while(add_idx < bank_member_change.nhs_to_add.size())
        {
            (*bank_fgnhg_map)[bank_member_change.nhs_to_add[add_idx]] = 
                std::vector<uint32_t>();
            auto it = bank_member_change.active_nhs.begin();
            while(bank_fgnhg_map->at(bank_member_change.nhs_to_add[add_idx]).size() != exp_bucket_size)
            {
                if(it == bank_member_change.active_nhs.end())
                {
                    it = bank_member_change.active_nhs.begin();
                }
                vector<uint32_t> *map_entry = &(bank_fgnhg_map->at(*it)); 
                if((*map_entry).size() <= 1)
                {

                    SWSS_LOG_WARN("Next-hop %s has %d entries, either number of buckets were less or we hit a bug",
                            (*it).to_string().c_str(), ((int)(*map_entry).size()));
                    return false;
                }
                else
                {
                    uint32_t last_elem = map_entry->at((*map_entry).size() - 1);
                    (*bank_fgnhg_map)[bank_member_change.nhs_to_add[add_idx]].push_back(last_elem);
                    (*map_entry).erase((*map_entry).end() - 1);

                    if(!write_hash_bucket_change_to_sai(syncd_fg_route_entry, last_elem, 
                        nhopgroup_members_set[bank_member_change.nhs_to_add[add_idx]],
                        ipPrefix, bank_member_change.nhs_to_add[add_idx]))
                    {
                        return false;
                    }
                }

                it++;
            }
            syncd_fg_route_entry->active_nexthops.insert(bank_member_change.nhs_to_add[add_idx]);
            add_idx++;
        }
    }
    return true;
}


bool FgNhgOrch::set_inactive_bank_to_next_available_active_bank(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        uint32_t bank, std::vector<Bank_Member_Changes> bank_member_changes,
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    uint32_t new_bank_idx = 0;
    for( ; new_bank_idx < bank_member_changes.size(); new_bank_idx++)
    {
        if(bank_member_changes[new_bank_idx].active_nhs.size() +
                bank_member_changes[new_bank_idx].nhs_to_add.size() != 0)
        {
            syncd_fg_route_entry->syncd_fgnhg_map[bank].clear();
            syncd_fg_route_entry->inactive_to_active_map[bank] = new_bank_idx;

            /* Create collated set of members which will be active in the bank */
            for(auto memb: bank_member_changes[new_bank_idx].nhs_to_add)
            {
                bank_member_changes[new_bank_idx].active_nhs.push_back(memb);
            }

            for(uint32_t i = fgNhgEntry->hash_bucket_indices[bank].start_index;
                i <= fgNhgEntry->hash_bucket_indices[bank].end_index; i++)
            {
                NextHopKey bank_nh_memb = bank_member_changes[new_bank_idx].
                         active_nhs[i % bank_member_changes[new_bank_idx].active_nhs.size()];

                if(!write_hash_bucket_change_to_sai(syncd_fg_route_entry, i,
                    nhopgroup_members_set[bank_nh_memb],ipPrefix, bank_nh_memb ))
                {
                    return false;
                }

                syncd_fg_route_entry->syncd_fgnhg_map[bank][bank_nh_memb].push_back(i);
            }
            break;
        }
    }

    if(new_bank_idx == bank_member_changes.size())
    {
        SWSS_LOG_NOTICE("No active next-hop members were found in any bank");
        /* Case where there are no active banks */
        /* Note: There is no way to set a NULL OID to the now inactive next-hops
         * so we leave the next-hops as is in SAI, and future route/neighbor changes
         * will take care of setting the next-hops to the correctly active nhs
         */
        syncd_fg_route_entry->syncd_fgnhg_map[bank].clear();
    }

    return true;
}


bool FgNhgOrch::set_inactive_bank_hash_bucket_changes(FGNextHopGroupEntry *syncd_fg_route_entry, FgNhgEntry *fgNhgEntry,
        uint32_t bank,std::vector<Bank_Member_Changes> &bank_member_changes, 
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set, const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    if(bank_member_changes[bank].nhs_to_add.size() > 0)
    {
        /* Previously inactive bank now transistions to active */
        syncd_fg_route_entry->syncd_fgnhg_map[bank].clear();
        for(uint32_t i = fgNhgEntry->hash_bucket_indices[bank].start_index;
                i <= fgNhgEntry->hash_bucket_indices[bank].end_index; i++)
        {
            NextHopKey bank_nh_memb = bank_member_changes[bank].
                nhs_to_add[i % bank_member_changes[bank].nhs_to_add.size()];

            if(!write_hash_bucket_change_to_sai(syncd_fg_route_entry, i, 
                  nhopgroup_members_set[bank_nh_memb], ipPrefix, bank_nh_memb))
            {
                return false;
            }

            syncd_fg_route_entry->syncd_fgnhg_map[bank][bank_nh_memb].push_back(i);
            syncd_fg_route_entry->active_nexthops.insert(bank_nh_memb);
        }
        syncd_fg_route_entry->inactive_to_active_map[bank] = bank;
    }
    else if(bank_member_changes[bank].nhs_to_del.size() > 0)
    {
        /* Previously active bank now transistions to inactive */
        if(!set_inactive_bank_to_next_available_active_bank(syncd_fg_route_entry, fgNhgEntry,
                    bank, bank_member_changes, nhopgroup_members_set, ipPrefix))
        {
            SWSS_LOG_INFO("Failed to map to active_bank and set nh in SAI");
            return false;
        }

        for(auto memb: bank_member_changes[bank].nhs_to_del)
        {
            syncd_fg_route_entry->active_nexthops.erase(memb);
        }
    }
    else
    {
        /* Previously inactive bank remains inactive */
        uint32_t active_bank = syncd_fg_route_entry->inactive_to_active_map[bank];
        if(bank_member_changes[active_bank].active_nhs.size() == 0)
        {
            if(!set_inactive_bank_to_next_available_active_bank(syncd_fg_route_entry, fgNhgEntry,
                        bank, bank_member_changes, nhopgroup_members_set, ipPrefix))
            {
                SWSS_LOG_INFO("Failed to map to active_bank and set nh in SAI");
                return false;
            }
        }
        else
        {
            if(!set_active_bank_hash_bucket_changes(syncd_fg_route_entry, fgNhgEntry, 
                active_bank, bank, bank_member_changes, nhopgroup_members_set, ipPrefix))
            {
                SWSS_LOG_INFO("Failed set_active_bank_hash_bucket_changes");
                return false;
            }
        }
    }
    return true;
}


bool FgNhgOrch::compute_and_set_hash_bucket_changes(FGNextHopGroupEntry *syncd_fg_route_entry, 
        FgNhgEntry *fgNhgEntry, std::vector<Bank_Member_Changes> &bank_member_changes, 
        std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set,
        const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    for(uint32_t bank_idx = 0; bank_idx < bank_member_changes.size(); bank_idx++)
    {
        if(bank_member_changes[bank_idx].active_nhs.size() == 0)
        {
            if(!set_inactive_bank_hash_bucket_changes(syncd_fg_route_entry, fgNhgEntry, 
                        bank_idx, bank_member_changes, nhopgroup_members_set, ipPrefix))
            {
                return false;
            }

        }
        else
        {
            if(!set_active_bank_hash_bucket_changes(syncd_fg_route_entry, fgNhgEntry, 
                        bank_idx, bank_idx, bank_member_changes, nhopgroup_members_set, ipPrefix))
            {
                return false;
            }
        }
    }
    return true;
}


bool FgNhgOrch::set_new_nhg_members(FGNextHopGroupEntry &syncd_fg_route_entry, FgNhgEntry *fgNhgEntry, 
        std::vector<Bank_Member_Changes> &bank_member_changes, std::map<NextHopKey,sai_object_id_t> &nhopgroup_members_set,
        const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    sai_status_t status;
    for(uint32_t i = 0; i < fgNhgEntry->hash_bucket_indices.size(); i++) 
    {
        uint32_t bank = i;
        syncd_fg_route_entry.inactive_to_active_map[bank] = bank;
        if(i + 1 > syncd_fg_route_entry.syncd_fgnhg_map.size())
        {
            syncd_fg_route_entry.syncd_fgnhg_map.push_back(FGNextHopGroupMap());
        }

        if(bank_member_changes[i].nhs_to_add.size() == 0)
        {
            /* Case where bank is empty */
            for(uint32_t active_bank = 0; active_bank < bank_member_changes.size(); active_bank++)
            {
                if(bank_member_changes[active_bank].nhs_to_add.size() != 0)
                {
                    bank = active_bank;
                    syncd_fg_route_entry.inactive_to_active_map[i] = active_bank;
                    break;
                }
            }
        } 

        if(bank_member_changes[bank].nhs_to_add.size() == 0)
        {
            /* Case where all banks are empty, we let retry logic(upon rv false) take care of this scenario */
            SWSS_LOG_INFO("Found no next-hops to add, skipping");
            return false;
        }

        for(uint32_t j = fgNhgEntry->hash_bucket_indices[i].start_index;
                j <= fgNhgEntry->hash_bucket_indices[i].end_index; j++)
        {
            NextHopKey bank_nh_memb = bank_member_changes[bank].nhs_to_add[j % 
                bank_member_changes[bank].nhs_to_add.size()];

            // Create a next hop group member
            sai_attribute_t nhgm_attr;
            vector<sai_attribute_t> nhgm_attrs;
            nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
            nhgm_attr.value.oid = syncd_fg_route_entry.next_hop_group_id;
            nhgm_attrs.push_back(nhgm_attr);

            nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
            nhgm_attr.value.oid = nhopgroup_members_set[bank_nh_memb];
            nhgm_attrs.push_back(nhgm_attr);

            nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_INDEX;
            nhgm_attr.value.s32 = j;
            nhgm_attrs.push_back(nhgm_attr);

            sai_object_id_t next_hop_group_member_id;
            status = sai_next_hop_group_api->create_next_hop_group_member(
                                                              &next_hop_group_member_id,
                                                              gSwitchId,
                                                              (uint32_t)nhgm_attrs.size(),
                                                              nhgm_attrs.data());
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to create next hop group %" PRIx64 " member %" PRIx64 ": %d\n",
                   syncd_fg_route_entry.next_hop_group_id, next_hop_group_member_id, status);
                
                if(!remove_nhg(&syncd_fg_route_entry, fgNhgEntry))
                {
                    SWSS_LOG_ERROR("Failed to clean-up after next-hop member creation failure");
                }
                
                return false;
            }

            set_state_db_route_entry(ipPrefix, j, bank_nh_memb);
            syncd_fg_route_entry.syncd_fgnhg_map[i][bank_nh_memb].push_back(j);
            syncd_fg_route_entry.active_nexthops.insert(bank_nh_memb);
            syncd_fg_route_entry.nhopgroup_members.push_back(next_hop_group_member_id);
            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MEMBER);
        }
    }

    return true;
}


bool FgNhgOrch::addRoute(sai_object_id_t vrf_id, const IpPrefix &ipPrefix, const NextHopGroupKey &nextHops)
{
    SWSS_LOG_ENTER();

    if (m_syncdFGRouteTables.find(vrf_id) != m_syncdFGRouteTables.end() &&
        m_syncdFGRouteTables.at(vrf_id).find(ipPrefix) != m_syncdFGRouteTables.at(vrf_id).end() &&
        m_syncdFGRouteTables.at(vrf_id).at(ipPrefix).nhg_key == nextHops)
    {
        return true;
    }

    if (m_syncdFGRouteTables.find(vrf_id) == m_syncdFGRouteTables.end())
    {
        m_syncdFGRouteTables.emplace(vrf_id, FGRouteTable());
        m_vrfOrch->increaseVrfRefCount(vrf_id);
    }

    auto prefix_entry = fgNhgPrefixes.find(ipPrefix);
    assert(prefix_entry != fgNhgPrefixes.end());
    FgNhgEntry *fgNhgEntry = prefix_entry->second;

    set<NextHopKey> next_hop_set = nextHops.getNextHops();
    std::map<NextHopKey,sai_object_id_t> nhopgroup_members_set;
    auto syncd_fg_route_entry_it = m_syncdFGRouteTables.at(vrf_id).find(ipPrefix);
    bool next_hop_to_add = false;

    /* Default init with # of banks */
    std::vector<Bank_Member_Changes> bank_member_changes(
            fgNhgEntry->hash_bucket_indices.size(), Bank_Member_Changes());
    if(fgNhgEntry->hash_bucket_indices.size() == 0)
    {
        /* Only happens the 1st time when hash_bucket_indices are not inited
         */
        for(auto it : fgNhgEntry->nextHops)
        {
            while(bank_member_changes.size() <= it.second)
            {
                bank_member_changes.push_back(Bank_Member_Changes());
            }
        }
    }

    /* Assert each IP address exists in m_syncdNextHops table,
     * and add the corresponding next_hop_id to next_hop_ids. */
    for (NextHopKey nhk : next_hop_set)
    {
        if (!m_neighOrch->hasNextHop(nhk))
        {
            SWSS_LOG_NOTICE("Failed to get next hop %s:%s in neighorch",
                    nhk.to_string().c_str(), nextHops.to_string().c_str());
            continue;
        }
        else if(fgNhgEntry->nextHops.find(nhk.ip_address) == fgNhgEntry->nextHops.end())
        {
            SWSS_LOG_WARN("Could not find next-hop %s in Fine Grained next-hop group entry for prefix %s, skipping",
                    nhk.to_string().c_str(), fgNhgEntry->fgNhg_name.c_str());
            continue;
        }
        else if (m_neighOrch->isNextHopFlagSet(nhk, NHFLAGS_IFDOWN))
        {
            SWSS_LOG_NOTICE("Next hop %s in %s is down, skipping",
                    nhk.to_string().c_str(), nextHops.to_string().c_str());
            continue;
        }

        if(syncd_fg_route_entry_it == m_syncdFGRouteTables.at(vrf_id).end())
        {
            bank_member_changes[fgNhgEntry->nextHops[nhk.ip_address]].
                nhs_to_add.push_back(nhk);
            next_hop_to_add = true;
        }
        else 
        {
            FGNextHopGroupEntry *syncd_fg_route_entry = &(syncd_fg_route_entry_it->second);
            if(syncd_fg_route_entry->active_nexthops.find(nhk) == 
                syncd_fg_route_entry->active_nexthops.end())
            {
                bank_member_changes[fgNhgEntry->nextHops[nhk.ip_address]].
                    nhs_to_add.push_back(nhk);
            }
        }

        sai_object_id_t next_hop_id = m_neighOrch->getNextHopId(nhk);
        nhopgroup_members_set[nhk] = next_hop_id;
    }

    if(syncd_fg_route_entry_it != m_syncdFGRouteTables.at(vrf_id).end())
    {
        FGNextHopGroupEntry *syncd_fg_route_entry = &(syncd_fg_route_entry_it->second);

        /* Route exists, update FG ECMP group in SAI */
        for(auto nhk : syncd_fg_route_entry->active_nexthops)
        {
            if(nhopgroup_members_set.find(nhk) == nhopgroup_members_set.end())
            {
                bank_member_changes[fgNhgEntry->nextHops[nhk.ip_address]].
                    nhs_to_del.push_back(nhk);
            }
            else
            {
                bank_member_changes[fgNhgEntry->nextHops[nhk.ip_address]].
                    active_nhs.push_back(nhk);
            }
        }

        if(!compute_and_set_hash_bucket_changes(syncd_fg_route_entry, fgNhgEntry, bank_member_changes, 
                nhopgroup_members_set, ipPrefix))
        {
            return false;
        }
    }
    else
    {
        /* New route + nhg addition */
        if(next_hop_to_add == false)
        {
            SWSS_LOG_INFO("There were no valid next-hops to add %s:%s", ipPrefix.to_string().c_str(),
                    nextHops.to_string().c_str());
            /* Let the route retry logic(upon false rc) take care of this case */
            return false;
        }

        if(gRouteOrch->getNextHopGroupCount() >= gRouteOrch->getMaxNextHopGroupCount())
        {
            SWSS_LOG_DEBUG("Failed to create new next hop group. \
                        Reached maximum number of next hop groups.");
            return false;
        }

        FGNextHopGroupEntry syncd_fg_route_entry;
        string platform = getenv("platform") ? getenv("platform") : "";

        sai_attribute_t nhg_attr;
        vector<sai_attribute_t> nhg_attrs;

        nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
        nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_FINE_GRAIN_ECMP;
        nhg_attrs.push_back(nhg_attr);

        nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_CONFIGURED_SIZE;
        nhg_attr.value.s32 = fgNhgEntry->configured_bucket_size;
        nhg_attrs.push_back(nhg_attr);

        sai_object_id_t next_hop_group_id;
        sai_status_t status = sai_next_hop_group_api->create_next_hop_group(&next_hop_group_id,
                                                                        gSwitchId,
                                                                        (uint32_t)nhg_attrs.size(),
                                                                        nhg_attrs.data());
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create next hop group %s, rv:%d",
                           nextHops.to_string().c_str(), status);
            return false;
        }

        gRouteOrch->incNextHopGroupCount();
        gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP);

        if(platform == VS_PLATFORM_SUBSTRING)
        {
           /* TODO: need implementation for SAI_NEXT_HOP_GROUP_ATTR_REAL_SIZE */ 
            fgNhgEntry->real_bucket_size = fgNhgEntry->configured_bucket_size;
        }
        else
        {
            nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_REAL_SIZE;
            nhg_attr.value.u32 = 0;
            status = sai_next_hop_group_api->get_next_hop_group_attribute(next_hop_group_id, 1, &nhg_attr);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to query next hop group %s SAI_NEXT_HOP_GROUP_ATTR_REAL_SIZE, rv:%d",
                           nextHops.to_string().c_str(), status);
                return false;
            }
            fgNhgEntry->real_bucket_size = nhg_attr.value.u32;
        }

        calculate_bank_hash_bucket_start_indices(fgNhgEntry);

        SWSS_LOG_NOTICE("fgnhgorch created next hop group %s of size %d", nextHops.to_string().c_str(), fgNhgEntry->real_bucket_size);

        syncd_fg_route_entry.next_hop_group_id = next_hop_group_id;

        if(!set_new_nhg_members(syncd_fg_route_entry, fgNhgEntry, bank_member_changes, nhopgroup_members_set, ipPrefix))
        {
            return false;
        }

        /* Now add the route pointing to the fgnhg */
        sai_route_entry_t route_entry;
        sai_attribute_t route_attr;
        route_entry.vr_id = vrf_id;
        route_entry.switch_id = gSwitchId;
        copy(route_entry.destination, ipPrefix);
        route_attr.id = SAI_ROUTE_ENTRY_ATTR_NEXT_HOP_ID;
        route_attr.value.oid = syncd_fg_route_entry.next_hop_group_id;
        status = sai_route_api->create_route_entry(&route_entry, 1, &route_attr);
        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create route %s with next hop(s) %s",
                    ipPrefix.to_string().c_str(), nextHops.to_string().c_str());

            /* Clean up the newly created next hop group entry */
            if(!remove_nhg(&syncd_fg_route_entry, fgNhgEntry))
            {
                SWSS_LOG_ERROR("Failed to clean-up after route creation failure");
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

        m_syncdFGRouteTables[vrf_id][ipPrefix] = syncd_fg_route_entry;

        SWSS_LOG_NOTICE("Created route %s:%s", ipPrefix.to_string().c_str(), nextHops.to_string().c_str());
    }
    m_syncdFGRouteTables[vrf_id][ipPrefix].nhg_key = nextHops; 

    for(uint32_t bank_idx = 0; bank_idx < bank_member_changes.size(); bank_idx++)
    {
        for(auto nh : bank_member_changes[bank_idx].nhs_to_add)
        {
            m_neighOrch->increaseNextHopRefCount(nh);
        }

        for(auto nh : bank_member_changes[bank_idx].nhs_to_del)
        {
            m_neighOrch->decreaseNextHopRefCount(nh);
        }
    }

    return true;
}


bool FgNhgOrch::removeRoute(sai_object_id_t vrf_id, const IpPrefix &ipPrefix)
{
    SWSS_LOG_ENTER();

    auto it_route_table = m_syncdFGRouteTables.find(vrf_id);
    if (it_route_table == m_syncdFGRouteTables.end())
    {
        SWSS_LOG_INFO("Failed to find route table, vrf_id 0x%" PRIx64 "\n", vrf_id);
        return true;
    }

    auto it_route = it_route_table->second.find(ipPrefix);
    if (it_route == it_route_table->second.end())
    {
        SWSS_LOG_INFO("Failed to find route entry, vrf_id 0x%" PRIx64 ", prefix %s\n", vrf_id,
                ipPrefix.to_string().c_str());
        return true;
    }

    auto prefix_entry = fgNhgPrefixes.find(ipPrefix);
    assert(prefix_entry != fgNhgPrefixes.end());
    FgNhgEntry *fgNhgEntry = prefix_entry->second;

    sai_route_entry_t route_entry;
    route_entry.vr_id = vrf_id;
    route_entry.switch_id = gSwitchId;
    copy(route_entry.destination, ipPrefix);
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

    FGNextHopGroupEntry *syncd_fg_route_entry = &(it_route->second);
    if(!remove_nhg(syncd_fg_route_entry, fgNhgEntry))
    {
        SWSS_LOG_ERROR("Failed to clean-up fine grained ECMP SAI group");
        return false;
    }

    for(auto nh : syncd_fg_route_entry->active_nexthops)
    {
        m_neighOrch->decreaseNextHopRefCount(nh);
    }

    // remove state_db entry
    m_stateWarmRestartRouteTable.del(ipPrefix.to_string());

    it_route_table->second.erase(it_route);
    return true;
}


bool FgNhgOrch::doTaskFgNhg(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();
    string op = kfvOp(t);
    string key = kfvKey(t);
    string fgNhg_name = key; 
    auto fgNhg_entry = m_FgNhgs.find(fgNhg_name);

    if (op == SET_COMMAND)
    {
        uint32_t bucket_size = 0;

        for (auto i : kfvFieldsValues(t))
        {
            if (fvField(i) == "bucket_size")
            {
                bucket_size = stoi(fvValue(i));
            }
        }

        if(bucket_size == 0)
        {
            SWSS_LOG_ERROR("Received bucket_size which is 0 for key %s", kfvKey(t).c_str());
            return true;
        }

        if(fgNhg_entry != m_FgNhgs.end()) 
        {
            if(bucket_size != (fgNhg_entry->second).configured_bucket_size)
            {
                SWSS_LOG_WARN("Received request to change %s's bucket size to %d, unsupported operation, skipping",
                        fgNhg_name.c_str(), bucket_size);
                return true;
            }
        }
        else
        {
            FgNhgEntry fgNhgEntry;
            fgNhgEntry.configured_bucket_size = bucket_size;
            fgNhgEntry.fgNhg_name = fgNhg_name;
            SWSS_LOG_INFO("%s: Added new FG_NHG entry with configured_bucket_size %d", 
                    __FUNCTION__,fgNhgEntry.configured_bucket_size);
            m_FgNhgs[fgNhg_name] = fgNhgEntry;
        }
    }
    else if (op == DEL_COMMAND)
    {
        if(fgNhg_entry == m_FgNhgs.end())
        {
            SWSS_LOG_INFO("%s: Received delete call for non-existent entry %s",
                    __FUNCTION__, fgNhg_name.c_str());
        }
        else 
        {
            /* delete all associated FG_NHG and SAI objects */
            if (fgNhg_entry->second.prefixes.size() == 0 && fgNhg_entry->second.nextHops.size() == 0)
            {
                m_FgNhgs.erase(fgNhg_entry);
                assert(m_FgNhgs.find(fgNhg_name) == fgNhgPrefixes.end());
                SWSS_LOG_INFO("%s: Received delete call for valid entry with no further dependencies, deleting %s",
                        __FUNCTION__, fgNhg_name.c_str());
            }
            else
            {
                SWSS_LOG_NOTICE("%s: still prefixes and nexthop in FG_NHG group %s", __FUNCTION__, fgNhg_name.c_str());
                return false;
            }
        }
    }
    return true;
}


bool FgNhgOrch::doTaskFgNhg_prefix(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();
    string op = kfvOp(t);
    string key = kfvKey(t);
    IpPrefix ip_prefix = IpPrefix(key);
    auto prefix_entry = fgNhgPrefixes.find(ip_prefix);
    bool route_handled = false;

    if (op == SET_COMMAND)
    {
        if(prefix_entry != fgNhgPrefixes.end())
        {
            SWSS_LOG_INFO("%s FG_NHG prefix already exists", __FUNCTION__);
            return true;
        }

        string fgNhg_name = "";
        for (auto i : kfvFieldsValues(t))
        {
            if (fvField(i) == "FG_NHG")
            {
                fgNhg_name = fvValue(i);
            }
        }
        if(fgNhg_name == "")
        {
            SWSS_LOG_ERROR("Received FG_NHG with empty name for key %s", kfvKey(t).c_str());
            return true;
        }

        auto fgNhg_entry = m_FgNhgs.find(fgNhg_name);
        if(fgNhg_entry == m_FgNhgs.end())
        {
            SWSS_LOG_INFO("%s FG_NHG entry not received yet, continue", __FUNCTION__);
            return false;
        }
        else 
        {
            /* delete regular ecmp handling for prefix */
            sai_object_id_t vrf_id = gVirtualRouterId;
            auto route_table_entry = gRouteOrch->getSyncdRoutes().find(vrf_id);
            NextHopGroupKey nhg;
            if (route_table_entry == gRouteOrch->getSyncdRoutes().end())
            {
                SWSS_LOG_INFO("Failed to find route table, vrf_id 0x%lx\n", vrf_id);
            }
            else
            {
                /* ipprefix already exist in routeorch */
                auto it_route = route_table_entry->second.find(ip_prefix);
                if (it_route != route_table_entry->second.end())
                {
                    nhg = it_route->second;
                    RouteBulkContext ctx;
                    ctx.ip_prefix = ip_prefix;
                    ctx.vrf_id = vrf_id;
                    ctx.nhg = nhg;
                    gRouteOrch->removeRoute(ctx);
                    gRouteOrch->gRouteBulker.flush();
                    gRouteOrch->removeRoutePost(ctx);
                    route_handled = true;
                }
            }

            fgNhg_entry->second.prefixes.push_back(ip_prefix);
            fgNhgPrefixes[ip_prefix] = &(fgNhg_entry->second);

            /* add fgnhg route */
            if (route_handled)
            {
                if(!addRoute(vrf_id, ip_prefix, nhg))
                {
                    SWSS_LOG_INFO("Failed to add fg route, %s:%s", 
                            ip_prefix.to_string().c_str(), nhg.to_string().c_str());
                    return false;
                }
                SWSS_LOG_INFO("%s: Add route with ip prefix %s with nexthop group key %s", 
                        __FUNCTION__, ip_prefix.to_string().c_str(),nhg.to_string().c_str());
            }

            SWSS_LOG_INFO("%s FG_NHG added for group %s, prefix %s",
                    __FUNCTION__, fgNhgPrefixes[ip_prefix]->fgNhg_name.c_str(), ip_prefix.to_string().c_str());
        }
       
    }
    else if (op == DEL_COMMAND)
    {
        if(prefix_entry == fgNhgPrefixes.end())
        {
            SWSS_LOG_INFO("%s FG_NHG prefix doesn't exists, ignore", __FUNCTION__);
            return true;
        }
        else
        {
            /* remove fgnhg route entry */
            sai_object_id_t vrf_id = gVirtualRouterId;
            NextHopGroupKey nhg;
            if (m_syncdFGRouteTables.find(vrf_id) != m_syncdFGRouteTables.end() &&
                    m_syncdFGRouteTables.at(vrf_id).find(ip_prefix) != m_syncdFGRouteTables.at(vrf_id).end())
            {
                nhg = m_syncdFGRouteTables.at(vrf_id).at(ip_prefix).nhg_key;
                if(!removeRoute(vrf_id, ip_prefix))
                {
                    SWSS_LOG_INFO("Failed to remove fg route, %s:%s", 
                                ip_prefix.to_string().c_str(), nhg.to_string().c_str());
                    return false;
                }
                route_handled = true;
                SWSS_LOG_INFO("%s FG_NHG prefix %s is removed from SAI",
                        __FUNCTION__, ip_prefix.to_string().c_str());
            }

            /* search and delete local structure */
            for (uint32_t i = 0; i < prefix_entry->second->prefixes.size(); i++)
            {
                if (prefix_entry->second->prefixes[i] == ip_prefix)
                {
                    prefix_entry->second->prefixes.erase(prefix_entry->second->prefixes.begin() + i);
                    SWSS_LOG_INFO("%s FG_NHG prefix %s is deleted from group %s",
                            __FUNCTION__, ip_prefix.to_string().c_str(), fgNhgPrefixes[ip_prefix]->fgNhg_name.c_str());
                    break;
                }
            }
            fgNhgPrefixes.erase(ip_prefix);

            /* reassign routeorch as the owner of the prefix and call for routeorch to add this route */
            if (route_handled)
            {
                RouteBulkContext ctx;
                ctx.ip_prefix = ip_prefix;
                ctx.vrf_id = vrf_id;
                ctx.nhg = nhg;
                gRouteOrch->addRoute(ctx, nhg);
                // Flush the route bulker, so routes will be written to syncd and ASIC
                gRouteOrch->gRouteBulker.flush();
                gRouteOrch->addRoutePost(ctx, nhg);
            }
        }
    }
    return true;
}


bool FgNhgOrch::doTaskFgNhg_member(const KeyOpFieldsValuesTuple & t)
{
    SWSS_LOG_ENTER();
    string op = kfvOp(t);
    string key = kfvKey(t);
    IpAddress next_hop = IpAddress(key);
    NextHopKey nhk(next_hop.to_string());

    if (op == SET_COMMAND)
    {
        string fgNhg_name = "";
        uint32_t bank = 0;
        for (auto i : kfvFieldsValues(t))
        {
            if (fvField(i) == "FG_NHG")
            {
                fgNhg_name = fvValue(i);
            }
            else if(fvField(i) == "bank")
            {
                bank = stoi(fvValue(i));
            }
        }
        if(fgNhg_name == "")
        {
            SWSS_LOG_ERROR("Received FG_NHG with empty name for key %s", kfvKey(t).c_str());
            return true;
        }

        auto fgNhg_entry = m_FgNhgs.find(fgNhg_name);
        if(fgNhg_entry == m_FgNhgs.end())
        {
            SWSS_LOG_INFO("%s FG_NHG entry not received yet, continue", __FUNCTION__);
            return false;
        }
        else
        {
            /* skip addition if next-hop already exists */
            if (fgNhg_entry->second.nextHops.find(next_hop) != fgNhg_entry->second.nextHops.end())
            {
                SWSS_LOG_INFO("%s FG_NHG member %s already exists, skip", __FUNCTION__, next_hop.to_string().c_str());
                return true;
            }
            fgNhg_entry->second.nextHops[next_hop] = bank;

            /* query and check the next hop is valid in neighOrcch */
            if (!m_neighOrch->hasNextHop(nhk))
            {
                SWSS_LOG_INFO("%s: Failed to get next hop %s in", __FUNCTION__, nhk.to_string().c_str());
                return true;
            }

            /* add next-hop into SAI group */
            if (!validnexthopinNextHopGroup(nhk))
            {
                return false;
            }
            SWSS_LOG_INFO("%s FG_NHG member added for group %s, next-hop %s",
                    __FUNCTION__, fgNhg_entry->second.fgNhg_name.c_str(), next_hop.to_string().c_str());
        }
    }
    else if (op == DEL_COMMAND)
    {
        /* remove next hop from SAI group */
        if (!invalidnexthopinNextHopGroup(nhk))
        {
            return false;
        }

        SWSS_LOG_INFO("%s FG_NHG member removed for SAI group, next-hop %s",
                __FUNCTION__, next_hop.to_string().c_str());

        /* remove next-hop in fgnhg entry*/
        for (auto fgnhg_it = m_FgNhgs.begin(); fgnhg_it != m_FgNhgs.end(); ++fgnhg_it)
        {
            auto it = fgnhg_it->second.nextHops.find(next_hop);
            if (it != fgnhg_it->second.nextHops.end())
            {
                SWSS_LOG_INFO("%s FG_NHG member removed for group %s, next-hop %s",
                        __FUNCTION__, fgnhg_it->second.fgNhg_name.c_str(), next_hop.to_string().c_str());
                fgnhg_it->second.nextHops.erase(it);
            }
        }
    }
    return true;
}
        

void FgNhgOrch::doTask(Consumer& consumer) {
    SWSS_LOG_ENTER();
    const string & table_name = consumer.getTableName();
    auto it = consumer.m_toSync.begin();
    bool entry_handled = true;

    while (it != consumer.m_toSync.end())
    {
        auto t = it->second;
        if(table_name == CFG_FG_NHG)
        {
            entry_handled = doTaskFgNhg(t);
        }
        else if(table_name == CFG_FG_NHG_PREFIX)
        {
            entry_handled = doTaskFgNhg_prefix(t);
        }
        else if(table_name == CFG_FG_NHG_MEMBER)
        {
            entry_handled = doTaskFgNhg_member(t);
        }
        else
        {
            entry_handled = true;
            SWSS_LOG_ERROR("%s Unknown table : %s", __FUNCTION__,table_name.c_str());
        }

        if (entry_handled)
        {
            consumer.m_toSync.erase(it++);
        }
        else
        {
            it++;
        }
    }
    return;
}
