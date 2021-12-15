#include "nhgmaporch.h"
#include "climits"
#include "crmorch.h"

extern sai_object_id_t gSwitchId;
extern sai_next_hop_group_api_t* sai_next_hop_group_api;
extern sai_switch_api_t *sai_switch_api;
extern CrmOrch *gCrmOrch;

uint64_t NhgMapOrch::m_max_nhg_map_count = 0;


NhgMapEntry::NhgMapEntry(sai_object_id_t _id, uint32_t _ref_count, int _largest_nh_index) :
    id(_id), ref_count(_ref_count), largest_nh_index(_largest_nh_index)
{
    SWSS_LOG_ENTER();
}

NhgMapOrch::NhgMapOrch(swss::DBConnector *db, const string &table_name) : Orch(db, table_name)
{
    SWSS_LOG_ENTER();

    /*
     * Get the maximum number of NHG maps.
     */
    if (sai_object_type_get_availability(gSwitchId,
                                        SAI_OBJECT_TYPE_NEXT_HOP_GROUP_MAP,
                                        0,
                                        nullptr,
                                        &m_max_nhg_map_count) != SAI_STATUS_SUCCESS)
    {
        SWSS_LOG_WARN("Switch does not support NHG maps");
        m_max_nhg_map_count = 0;
    }
}

void NhgMapOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        swss::KeyOpFieldsValuesTuple t = it->second;
        string index = kfvKey(t);
        string op = kfvOp(t);
        bool success = true;

        auto fc_map_it = m_syncdMaps.find(index);

        /*
         * Set operation.
         */
        if (op == SET_COMMAND)
        {
            /*
             * Extract the map from the values.
             */
            auto p = getMap(kfvFieldsValues(t));
            success = p.first;

            /*
             * If the map can't be extracted, erase the work item as it's useless to retry unless the user updates the
             * wrong values. We achieve the erase by setting the success value to "true".
             *
             */
            if (!success)
            {
                SWSS_LOG_ERROR("Failed to extract NHG map %s", index.c_str());
                success = true;
            }
            else
            {
                /*
                * Create the SAI map.  Also track the largest index referenced by the map as we do.
                */
                auto *fc_map = new sai_map_t[p.second.size()];
                uint32_t ii = 0;
                int largest_nh_index = 0;

                for (const auto &fc_nh_idx : p.second)
                {
                    fc_map[ii].key = fc_nh_idx.first;
                    fc_map[ii++].value = fc_nh_idx.second;

                    if (fc_nh_idx.second > largest_nh_index)
                    {
                        largest_nh_index = fc_nh_idx.second;
                    }
                }

                sai_map_list_t fc_map_list;
                assert(p.second.size() <= UINT32_MAX);
                fc_map_list.count = static_cast<uint32_t>(p.second.size());
                fc_map_list.list = fc_map;

                /*
                 * Create the map.
                 */
                if (fc_map_it == m_syncdMaps.end())
                {
                    /*
                     * Check if we have enough resources for a new map.
                     */
                    if (m_syncdMaps.size() >= m_max_nhg_map_count)
                    {
                        SWSS_LOG_WARN("No more resources left for new next hop group map %s", index.c_str());
                        success = false;
                    }
                    else
                    {
                        /*
                        * Set the map attributes.
                        */
                        sai_attribute_t attr;
                        vector<sai_attribute_t> attrs;

                        attr.id = SAI_NEXT_HOP_GROUP_MAP_ATTR_TYPE;
                        attr.value.u32 = SAI_NEXT_HOP_GROUP_MAP_TYPE_FORWARDING_CLASS_TO_INDEX;
                        attrs.push_back(move(attr));

                        attr.id = SAI_NEXT_HOP_GROUP_MAP_ATTR_MAP_TO_VALUE_LIST;
                        attr.value.maplist = fc_map_list;
                        attrs.push_back(move(attr));

                        /*
                        * Create the map over SAI.
                        */
                        sai_object_id_t nhg_map_id;
                        sai_status_t status = sai_next_hop_group_api->create_next_hop_group_map(&nhg_map_id,
                                                                                gSwitchId,
                                                                                static_cast<uint32_t>(attrs.size()),
                                                                                attrs.data());

                        if (status != SAI_STATUS_SUCCESS)
                        {
                            SWSS_LOG_ERROR("Failed to create NHG map %s, rv %d", index.c_str(), status);
                            success = false;
                        }
                        else
                        {
                            assert(nhg_map_id != SAI_NULL_OBJECT_ID);
                            NhgMapEntry entry(nhg_map_id, 0, largest_nh_index);
                            m_syncdMaps.emplace(move(index), entry);

                            gCrmOrch->incCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MAP);
                        }
                    }
                }
                /*
                 * Update the map.
                 */
                else
                {
                    /*
                     * Update the map attribute.
                     */
                    sai_attribute_t attr;
                    attr.id = SAI_NEXT_HOP_GROUP_MAP_ATTR_MAP_TO_VALUE_LIST;
                    attr.value.maplist = fc_map_list;

                    sai_status_t status = sai_next_hop_group_api->set_next_hop_group_map_attribute(fc_map_it->second.id,
                                                                                                    &attr);

                    if (status != SAI_STATUS_SUCCESS)
                    {
                        SWSS_LOG_ERROR("Failed to update NHG map %s, rv %d", index.c_str(), status);
                        success = false;
                    }
                }

                /*
                 * Free the allocated memory.
                 */
                delete[] fc_map;
            }
        }
        /*
         * Del operation.
         */
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
            else if (fc_map_it == m_syncdMaps.end())
            {
                SWSS_LOG_WARN("NHG map %s does not exist to deleted", index.c_str());
            }
            else if (fc_map_it->second.ref_count > 0)
            {
                SWSS_LOG_WARN("Can not delete referenced NHG map %s", index.c_str());
                success = false;
            }
            else
            {
                sai_status_t status = sai_next_hop_group_api->remove_next_hop_group_map(fc_map_it->second.id);

                if (status == SAI_STATUS_SUCCESS)
                {
                    m_syncdMaps.erase(fc_map_it);
                    gCrmOrch->decCrmResUsedCounter(CrmResourceType::CRM_NEXTHOP_GROUP_MAP);
                }
                else
                {
                    SWSS_LOG_ERROR("Failed to remove NHG map %s, rv %d", index.c_str(), status);
                    success = false;
                }
            }
        }
        else
        {
            SWSS_LOG_WARN("Unknown operation type %s", op.c_str());

            /*
             * Mark the operation as a success to remove the task.
             */
            success = true;
        }

        /*
         * Depending on the operation success, remove the task or skip it.
         */
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
 * Return the SAI ID for the map indexed by "index".  If it  does not exist, return SAI_NULL_OBJECT_ID.
 */
sai_object_id_t NhgMapOrch::getMapId(const string &index) const
{
    SWSS_LOG_ENTER();

    auto it = m_syncdMaps.find(index);

    return it == m_syncdMaps.end() ? SAI_NULL_OBJECT_ID : it->second.id;
}

/*
 * Return the largest NH index used by the map indexed by "index".  If it  does not exist, return 0.
 */
int NhgMapOrch::getLargestNhIndex(const string &index) const
{
    SWSS_LOG_ENTER();

    auto it = m_syncdMaps.find(index);

    return it == m_syncdMaps.end() ? 0 : it->second.largest_nh_index;
}

/*
 * Increase reference counter for a map.
 */
void NhgMapOrch::incRefCount(const string &index)
{
    SWSS_LOG_ENTER();

    ++m_syncdMaps.at(index).ref_count;
}

/*
 * Decrease reference counter for a map.
 */
void NhgMapOrch::decRefCount(const string &index)
{
    SWSS_LOG_ENTER();

    auto &nhg_map = m_syncdMaps.at(index);

    if (nhg_map.ref_count == 0)
    {
        SWSS_LOG_ERROR("Decreasing reference counter beyond 0 for NHG map %s", index.c_str());
        throw std::runtime_error("Decreasing reference counter beyond 0");
    }

    --nhg_map.ref_count;
}

/*
 * Get the maximum number of FC classes supported by the switch.
 */
sai_uint8_t NhgMapOrch::getMaxNumFcs()
{
    SWSS_LOG_ENTER();

    static int max_num_fcs = -1;

    /*
     * Get the maximum number of FC classes if it wasn't already initialized.
     */
    if (max_num_fcs == -1)
    {
        sai_attribute_t attr;
        attr.id = SAI_SWITCH_ATTR_MAX_NUMBER_OF_FORWARDING_CLASSES;

        if (sai_switch_api->get_switch_attribute(gSwitchId, 1, &attr) == SAI_STATUS_SUCCESS)
        {
            max_num_fcs = attr.value.u8;
        }
        else
        {
            SWSS_LOG_WARN("Switch does not support FCs");
            max_num_fcs = 0;
        }
    }

    return static_cast<sai_uint8_t>(max_num_fcs);
}

/*
 * Extract the NHG map from the FV tuples
 */
pair<bool, unordered_map<sai_uint32_t, sai_int32_t>> NhgMapOrch::getMap(const vector<swss::FieldValueTuple> &values)
{
    SWSS_LOG_ENTER();

    bool success = true;

    /*
     * If the map is empty, return error
     */
    if (values.empty())
    {
        SWSS_LOG_ERROR("NHG map is empty");
        success = false;
    }

    unordered_map<sai_uint32_t, sai_int32_t> fc_map;
    sai_uint8_t max_num_fcs = getMaxNumFcs();

    /*
    * Create the map while validating that the values are positive
    */
    for (auto it = values.begin(); it != values.end(); ++it)
    {
        try
        {
            /*
             * Check the FC value is valid. FC value must be in range [0, max_num_fcs).
             */
            auto fc = stoi(fvField(*it));

            if ((fc < 0) || (fc >= max_num_fcs))
            {
                SWSS_LOG_ERROR("FC value %d is either negative or greater than max value %d", fc, max_num_fcs - 1);
                success = false;
                break;
            }

            /*
             * Check the NH index value is valid.
             */
            auto nh_idx = stoi(fvValue(*it));

            if (nh_idx < 0)
            {
                SWSS_LOG_ERROR("NH index %d is negative", nh_idx);
                success = false;
                break;
            }

            fc_map.emplace(fc, nh_idx).second;
        }
        catch(const invalid_argument& e)
        {
            SWSS_LOG_ERROR("Got exception during conversion: %s", e.what());
            success = false;
            break;
        }
    }

    return {success, fc_map};
}
