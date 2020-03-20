#include <assert.h>
#include <deque>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <boost/functional/hash.hpp>
#include <sairedis.h>
#include "sai.h"

/*
static inline bool operator==(const sai_ip_prefix_t& a, const sai_ip_prefix_t& b)
{
    if (a.addr_family != b.addr_family) return false;

    if (a.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        return a.addr.ip4 == b.addr.ip4
            && a.mask.ip4 == b.mask.ip4
            ;
    }
    else if (a.addr_family == SAI_IP_ADDR_FAMILY_IPV6)
    {
        return memcmp(a.addr.ip6, b.addr.ip6, sizeof(a.addr.ip6)) == 0
            && memcmp(a.mask.ip6, b.mask.ip6, sizeof(a.mask.ip6)) == 0
            ;
    }
    else
    {
        throw std::invalid_argument("a has invalid addr_family");
    }
}

static inline bool operator==(const sai_route_entry_t& a, const sai_route_entry_t& b)
{
    return a.switch_id == b.switch_id
        && a.vr_id == b.vr_id
        && a.destination == b.destination
        ;
}
*/

static inline std::size_t hash_value(const sai_ip_prefix_t& a)
{
    size_t seed = 0;
    boost::hash_combine(seed, a.addr_family);
    if (a.addr_family == SAI_IP_ADDR_FAMILY_IPV4)
    {
        boost::hash_combine(seed, a.addr.ip4);
        boost::hash_combine(seed, a.mask.ip4);
    }
    else if (a.addr_family == SAI_IP_ADDR_FAMILY_IPV6)
    {
        boost::hash_combine(seed, a.addr.ip6);
        boost::hash_combine(seed, a.mask.ip6);
    }
    return seed;
}

namespace std
{
    template <>
    struct hash<sai_route_entry_t>
    {
        size_t operator()(const sai_route_entry_t& a) const noexcept
        {
            size_t seed = 0;
            boost::hash_combine(seed, a.switch_id);
            boost::hash_combine(seed, a.vr_id);
            boost::hash_combine(seed, a.destination);
            return seed;
        }
    };
    
    template <>
    struct hash<sai_fdb_entry_t>
    {
        size_t operator()(const sai_fdb_entry_t& a) const noexcept
        {
            size_t seed = 0;
            boost::hash_combine(seed, a.switch_id);
            boost::hash_combine(seed, a.mac_address);
            boost::hash_combine(seed, a.bv_id);
            return seed;
        }
    };
}

/*
struct NextHopGroupEntry
{
    sai_object_id_t             next_hop_group_id;      // next hop group id
    std::set<sai_object_id_t>   next_hop_group_members; // next hop group member ids
    int                         ref_count;              // reference count
};
*/

/* NextHopGroupTable: next hop group IP addersses, NextHopGroupEntry */
//typedef std::map<IpAddresses, NextHopGroupEntry> NextHopGroupTable;

// SAI typedef which is not available in SAI 1.5
// TODO: remove after available
typedef sai_status_t (*sai_bulk_create_fdb_entry_fn)(
        _In_ uint32_t object_count,
        _In_ const sai_fdb_entry_t *fdb_entry,
        _In_ const uint32_t *attr_count,
        _In_ const sai_attribute_t **attr_list,
        _In_ sai_bulk_op_error_mode_t mode,
        _Out_ sai_status_t *object_statuses);
typedef sai_status_t (*sai_bulk_remove_fdb_entry_fn)(
        _In_ uint32_t object_count,
        _In_ const sai_fdb_entry_t *fdb_entry,
        _In_ sai_bulk_op_error_mode_t mode,
        _Out_ sai_status_t *object_statuses);
typedef sai_status_t (*sai_bulk_set_fdb_entry_attribute_fn)(
        _In_ uint32_t object_count,
        _In_ const sai_fdb_entry_t *fdb_entry,
        _In_ const sai_attribute_t *attr_list,
        _In_ sai_bulk_op_error_mode_t mode,
        _Out_ sai_status_t *object_statuses);
        
template<typename T>
struct saitraits
{
};

template<>
struct saitraits<sai_route_entry_t>
{
    using api_t = sai_route_api_t;
    using create_entry_fn = sai_create_route_entry_fn;
    using remove_entry_fn = sai_remove_route_entry_fn;
    using set_entry_attribute_fn = sai_set_route_entry_attribute_fn;
    using bulk_create_entry_fn = sai_bulk_create_route_entry_fn;
    using bulk_remove_entry_fn = sai_bulk_remove_route_entry_fn;
    using bulk_set_entry_attribute_fn = sai_bulk_set_route_entry_attribute_fn;
};

template<>
struct saitraits<sai_fdb_entry_t>
{
    using api_t = sai_fdb_api_t;
    using create_entry_fn = sai_create_fdb_entry_fn;
    using remove_entry_fn = sai_remove_fdb_entry_fn;
    using set_entry_attribute_fn = sai_set_fdb_entry_attribute_fn;
    using bulk_create_entry_fn = sai_bulk_create_fdb_entry_fn;
    using bulk_remove_entry_fn = sai_bulk_remove_fdb_entry_fn;
    using bulk_set_entry_attribute_fn = sai_bulk_set_fdb_entry_attribute_fn;
};

/*
template<>
struct saitraits<sai_next_hop_group_api_t>
{
    using api_t = sai_fdb_api_t;
    using create_entry_fn = sai_create_fdb_entry_fn;
    using remove_entry_fn = sai_remove_fdb_entry_fn;
    using set_entry_attribute_fn = sai_set_fdb_entry_attribute_fn;
    using bulk_create_entry_fn = sai_bulk_create_fdb_entry_fn;
    using bulk_remove_entry_fn = sai_bulk_remove_fdb_entry_fn;
    using bulk_set_entry_attribute_fn = sai_bulk_set_fdb_entry_attribute_fn;
};
*/

template <typename T, typename Ts = saitraits<T>>
class RouteBulker
{
public:
    RouteBulker(typename Ts::api_t *api/*, sai_object_id_t switch_id = SAI_NULL_OBJECT_ID*/);

    sai_status_t create_route_entry(
        _In_ const T *route_entry,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        creating_entries.emplace(std::piecewise_construct,
                std::forward_as_tuple(*route_entry),
                std::forward_as_tuple(attr_list, attr_list + attr_count));
        SWSS_LOG_DEBUG("bulk.create_route_entry %zu, %zu, %d\n", creating_entries.size(), creating_entries[*route_entry].size(), (int)creating_entries[*route_entry][0].id);
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t remove_route_entry(
        _In_ const T *route_entry)
    {
        assert(route_entry);
        if (!route_entry) throw std::invalid_argument("route_entry is null");

        auto found_setting = setting_entries.find(*route_entry);
        if (found_setting != setting_entries.end())
        {
            setting_entries.erase(found_setting);
        }

        auto found_creating = creating_entries.find(*route_entry);
        if (found_creating != creating_entries.end())
        {
            creating_entries.erase(found_creating);
        }
        else
        {
            removing_entries.emplace(*route_entry);
        }

        return SAI_STATUS_SUCCESS;
    }

    sai_status_t set_route_entry_attribute(
        _In_ const T *route_entry,
        _In_ const sai_attribute_t *attr)
    {
        auto found_setting = setting_entries.find(*route_entry);
        if (found_setting != setting_entries.end())
        {
            // For simplicity, just insert new attribute at the vector end, no merging
            found_setting->second.emplace_back(*attr);
        }
        else
        {
            // Create a new key if not exists in the map
            setting_entries.emplace(std::piecewise_construct,
                std::forward_as_tuple(*route_entry),
                std::forward_as_tuple(1, *attr));
        }

        return SAI_STATUS_SUCCESS;
    }

    void flush()
    {
        // Removing
        if (!removing_entries.empty())
        {
            vector<T> rs;
            
            for (auto i: removing_entries)
            {
                auto& route_entry = i;
                rs.push_back(route_entry);
            }
            uint32_t route_count = (uint32_t)removing_entries.size();
            vector<sai_status_t> statuses(route_count);
            (*remove_route_entries)(route_count, rs.data(), SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());
            
            SWSS_LOG_NOTICE("bulk.flush removing_entries %zu\n", removing_entries.size());
            
            removing_entries.clear();
        }

        // Creating
        if (!creating_entries.empty())
        {
            vector<T> rs;
            vector<sai_attribute_t const*> tss;
            vector<uint32_t> cs;

            for (auto const& i: creating_entries)
            {
                auto const& route_entry = i.first;
                auto const& attrs = i.second;

                rs.push_back(route_entry);
                tss.push_back(attrs.data());
                cs.push_back((uint32_t)attrs.size());
            }
            uint32_t route_count = (uint32_t)creating_entries.size();
            vector<sai_status_t> statuses(route_count);
            (*create_route_entries)(route_count, rs.data(), cs.data(), tss.data()
                , SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());

            SWSS_LOG_NOTICE("bulk.flush creating_entries %zu\n", creating_entries.size());

            creating_entries.clear();
        }

        // Setting
        if (!setting_entries.empty())
        {
            vector<T> rs;
            vector<sai_attribute_t> ts;
            vector<uint32_t> cs;

            for (auto const& i: setting_entries)
            {
                auto const& route_entry = i.first;
                auto const& attrs = i.second;
                for (auto const& attr: attrs)
                {
                    rs.push_back(route_entry);
                    ts.push_back(attr);
                }
            }
            uint32_t route_count = (uint32_t)setting_entries.size();
            vector<sai_status_t> statuses(route_count);
            (*set_route_entries_attribute)(route_count, rs.data(), ts.data()
                , SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());

            SWSS_LOG_NOTICE("bulk.flush setting_entries %zu\n", setting_entries.size());

            setting_entries.clear();
        }
    }

    void clear()
    {
        removing_entries.clear();
        creating_entries.clear();
        setting_entries.clear();
    }

private:
    sai_object_id_t                                         switch_id = SAI_NULL_OBJECT_ID;
    std::unordered_map<T, std::vector<sai_attribute_t>>     creating_entries;
    std::unordered_map<T, std::vector<sai_attribute_t>>     setting_entries;
    std::unordered_set<T>                                   removing_entries;
    
    typename Ts::bulk_create_entry_fn                       create_route_entries;
    typename Ts::bulk_remove_entry_fn                       remove_route_entries;
    typename Ts::bulk_set_entry_attribute_fn                set_route_entries_attribute;
};

template <typename T, typename Ts>
RouteBulker<T, Ts>::RouteBulker(typename Ts::api_t *api)
{
    throw std::logic_error("Not implemented");
}

template <>
RouteBulker<sai_route_entry_t>::RouteBulker(saitraits<sai_route_entry_t>::api_t *api)
{
    create_route_entries = api->create_route_entries;
    remove_route_entries = api->remove_route_entries;
    set_route_entries_attribute = api->set_route_entries_attribute;
}


template <>
RouteBulker<sai_fdb_entry_t>::RouteBulker(saitraits<sai_fdb_entry_t>::api_t *api)
{
    // TODO: implement after create_fdb_entries() is available in SAI
    throw std::logic_error("Not implemented");
    /*
    create_route_entries = api->create_fdb_entries;
    remove_route_entries = api->remove_fdb_entries;
    set_route_entries_attribute = api->set_fdb_entries_attribute;
    */
}

#if 0
class NextHopGroupBulker
{
public:
    NextHopGroupBulker(sai_next_hop_group_api_t* next_hop_group_api, RouteBulker* routebulker, int maxNextHopGroupCount, sai_object_id_t switchId)
        : sai_next_hop_group_api(next_hop_group_api)
        , m_nextHopGroupCount(0)
        , route_bulker(routebulker)
        , m_maxNextHopGroupCount(maxNextHopGroupCount)
        , m_switchId(switchId)
    {
    }

    bool hasNextHopGroup(const IpAddresses& ipAddresses) const
    {
        return m_syncdNextHopGroups.find(ipAddresses) != m_syncdNextHopGroups.end();
    }

    sai_object_id_t getNextHopGroupId(const IpAddresses& ipAddresses)
    {
        assert(hasNextHopGroup(ipAddresses));
        return m_syncdNextHopGroups[ipAddresses].next_hop_group_id;
    }

    void increaseNextHopRefCount(const IpAddresses& ipAddresses)
    {
        assert(ipAddresses.getSize() > 1);
        m_syncdNextHopGroups[ipAddresses].ref_count ++;
    }
    void decreaseNextHopRefCount(const IpAddresses& ipAddresses)
    {
        assert(ipAddresses.getSize() > 1);
        m_syncdNextHopGroups[ipAddresses].ref_count --;
    }

    bool isRefCounterZero(const IpAddresses& ipAddresses) const
    {
        if (!hasNextHopGroup(ipAddresses))
        {
            return true;
        }

        return m_syncdNextHopGroups.at(ipAddresses).ref_count == 0;
    }

    bool addNextHopGroup(const IpAddresses& ipAddresses, const vector<sai_object_id_t>& next_hop_ids)
    {
        if (m_nextHopGroupCount >= m_maxNextHopGroupCount)
        {
            SWSS_LOG_DEBUG("Failed to create new next hop group. \
                            Reaching maximum number of next hop groups.");
            return false;
        }

        route_bulker->flush();

        sai_attribute_t nhg_attr;
        vector<sai_attribute_t> nhg_attrs;

        nhg_attr.id = SAI_NEXT_HOP_GROUP_ATTR_TYPE;
        nhg_attr.value.s32 = SAI_NEXT_HOP_GROUP_TYPE_ECMP;
        nhg_attrs.push_back(nhg_attr);

        sai_object_id_t next_hop_group_id;
        sai_status_t status = sai_next_hop_group_api->
                create_next_hop_group(&next_hop_group_id, m_switchId, (uint32_t)nhg_attrs.size(), nhg_attrs.data());

        if (status != SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_ERROR("Failed to create next hop group %s, rv:%d",
                           ipAddresses.to_string().c_str(), status);
            return false;
        }

        m_nextHopGroupCount ++;
        SWSS_LOG_NOTICE("Create next hop group %s", ipAddresses.to_string().c_str());

        NextHopGroupEntry next_hop_group_entry;
        next_hop_group_entry.next_hop_group_id = next_hop_group_id;

        uint32_t object_count = (uint32_t)next_hop_ids.size();
        vector<uint32_t> attr_count;
        vector<vector<sai_attribute_t>> attrs;
        vector<sai_object_id_t> object_id(object_count);
        vector<sai_status_t> object_statuses(object_count);

        // Create a next hop group members
        for (auto nhid: next_hop_ids)
        {
            attrs.emplace_back();
            auto& nhgm_attrs = attrs.back();

            sai_attribute_t nhgm_attr;
            nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_GROUP_ID;
            nhgm_attr.value.oid = next_hop_group_id;
            nhgm_attrs.push_back(nhgm_attr);

            nhgm_attr.id = SAI_NEXT_HOP_GROUP_MEMBER_ATTR_NEXT_HOP_ID;
            nhgm_attr.value.oid = nhid;
            nhgm_attrs.push_back(nhgm_attr);

            attr_count.push_back((uint32_t)nhgm_attrs.size());
        }

        vector<sai_attribute_t const*> attrs_array;
        for (auto const& i: attrs)
        {
            attrs_array.push_back(i.data());
        }
        status = sai_next_hop_group_api->create_next_hop_group_members(m_switchId, object_count, attr_count.data(), attrs_array.data()
            , SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, object_id.data(), object_statuses.data());

        for (uint32_t idx = 0; idx < object_count; idx++)
        {
            sai_object_id_t next_hop_group_member_id = object_id[idx];
            if (status != SAI_STATUS_SUCCESS)
            {
                // TODO: do we need to clean up?
                SWSS_LOG_ERROR("Failed to create next hop group %lx member %lx: %d\n",
                               next_hop_group_id, next_hop_group_member_id, status);
                return false;
            }

        }
        // Save the membership into next hop structure
        next_hop_group_entry.next_hop_group_members.insert(object_id.begin(), object_id.end());

        /*
         * Initialize the next hop group structure with ref_count as 0. This
         * count will increase once the route is successfully syncd.
         */
        next_hop_group_entry.ref_count = 0;
        m_syncdNextHopGroups[ipAddresses] = next_hop_group_entry;
        return true;
    }

    bool removeNextHopGroup(const IpAddresses& ipAddresses)
    {
        sai_status_t status;
        assert(hasNextHopGroup(ipAddresses));

        route_bulker->flush();
        if (m_syncdNextHopGroups[ipAddresses].ref_count == 0)
        {
            auto next_hop_group_entry = m_syncdNextHopGroups[ipAddresses];
            sai_object_id_t next_hop_group_id = next_hop_group_entry.next_hop_group_id;

            for (auto next_hop_group_member_id: next_hop_group_entry.next_hop_group_members)
            {
                status = sai_next_hop_group_api->remove_next_hop_group_member(next_hop_group_member_id);
                if (status != SAI_STATUS_SUCCESS)
                {
                    SWSS_LOG_ERROR("Failed to remove next hop group member %lx, rv:%d", next_hop_group_member_id, status);
                    return false;
                }
            }

            sai_status_t status = sai_next_hop_group_api->remove_next_hop_group(next_hop_group_id);
            if (status != SAI_STATUS_SUCCESS)
            {
                SWSS_LOG_ERROR("Failed to remove next hop group %lx, rv:%d", next_hop_group_id, status);
                return false;
            }

            m_nextHopGroupCount --;
            m_syncdNextHopGroups.erase(ipAddresses);
            return true;
        }
        else
        {
            return false;
        }
    }

private:
    int m_nextHopGroupCount;
    int m_maxNextHopGroupCount;
    NextHopGroupTable m_syncdNextHopGroups;
    sai_next_hop_group_api_t *                                              sai_next_hop_group_api;
    RouteBulker * route_bulker;
    sai_object_id_t m_switchId;
};
#endif
