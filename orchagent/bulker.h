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
struct saitraits { };

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

template<>
struct saitraits<sai_next_hop_group_api_t>
{
    using api_t = sai_next_hop_group_api_t;
    using create_entry_fn = sai_create_next_hop_group_member_fn;
    using remove_entry_fn = sai_remove_next_hop_group_member_fn;
    using set_entry_attribute_fn = sai_set_next_hop_group_member_attribute_fn;
    using bulk_create_entry_fn = sai_bulk_object_create_fn;
    using bulk_remove_entry_fn = sai_bulk_object_remove_fn;
    // TODO: wait until available in SAI
    //using bulk_set_entry_attribute_fn = sai_bulk_object_set_attribute_fn;
};

template <typename T, typename Ts = saitraits<T>>
class RouteBulker
{
public:
    RouteBulker(typename Ts::api_t *api)
    {
        throw std::logic_error("Not implemented");
    }

    sai_status_t create_entry(
        _In_ const T *entry,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        auto rc = creating_entries.emplace(std::piecewise_construct,
                std::forward_as_tuple(*entry),
                std::forward_as_tuple(attr_list, attr_list + attr_count));
        bool inserted = rc.second;
        SWSS_LOG_DEBUG("bulk.create_entry %zu, %zu, %d, %d\n", creating_entries.size(), creating_entries[*entry].size(), (int)creating_entries[*entry][0].id, inserted);
        return inserted ? SAI_STATUS_SUCCESS : SAI_STATUS_ITEM_ALREADY_EXISTS;
    }

    sai_status_t remove_entry(
        _In_ const T *entry)
    {
        assert(entry);
        if (!entry) throw std::invalid_argument("entry is null");

        auto found_setting = setting_entries.find(*entry);
        if (found_setting != setting_entries.end())
        {
            setting_entries.erase(found_setting);
        }

        auto found_creating = creating_entries.find(*entry);
        if (found_creating != creating_entries.end())
        {
            creating_entries.erase(found_creating);
        }
        else
        {
            removing_entries.emplace(*entry);
        }

        return SAI_STATUS_SUCCESS;
    }

    sai_status_t set_entry_attribute(
        _In_ const T *entry,
        _In_ const sai_attribute_t *attr)
    {
        auto found_setting = setting_entries.find(*entry);
        if (found_setting != setting_entries.end())
        {
            // For simplicity, just insert new attribute at the vector end, no merging
            found_setting->second.emplace_back(*attr);
        }
        else
        {
            // Create a new key if not exists in the map
            setting_entries.emplace(std::piecewise_construct,
                std::forward_as_tuple(*entry),
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
                auto& entry = i;
                rs.push_back(entry);
            }
            uint32_t route_count = (uint32_t)removing_entries.size();
            vector<sai_status_t> statuses(route_count);
            (*remove_entries)(route_count, rs.data(), SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());
            
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
                auto const& entry = i.first;
                auto const& attrs = i.second;

                rs.push_back(entry);
                tss.push_back(attrs.data());
                cs.push_back((uint32_t)attrs.size());
            }
            uint32_t route_count = (uint32_t)creating_entries.size();
            vector<sai_status_t> statuses(route_count);
            (*create_entries)(route_count, rs.data(), cs.data(), tss.data()
                , SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());

            SWSS_LOG_NOTICE("bulk.flush creating_entries %zu\n", creating_entries.size());

            creating_entries.clear();
        }

        // Setting
        if (!setting_entries.empty())
        {
            vector<T> rs;
            vector<sai_attribute_t> ts;

            for (auto const& i: setting_entries)
            {
                auto const& entry = i.first;
                auto const& attrs = i.second;
                for (auto const& attr: attrs)
                {
                    rs.push_back(entry);
                    ts.push_back(attr);
                }
            }
            uint32_t route_count = (uint32_t)setting_entries.size();
            vector<sai_status_t> statuses(route_count);
            (*set_entries_attribute)(route_count, rs.data(), ts.data()
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
    std::unordered_map<T, std::vector<sai_attribute_t>>     creating_entries;
    std::unordered_map<T, std::vector<sai_attribute_t>>     setting_entries;
    std::unordered_set<T>                                   removing_entries;
    
    typename Ts::bulk_create_entry_fn                       create_entries;
    typename Ts::bulk_remove_entry_fn                       remove_entries;
    typename Ts::bulk_set_entry_attribute_fn                set_entries_attribute;
};

template <>
RouteBulker<sai_route_entry_t>::RouteBulker(saitraits<sai_route_entry_t>::api_t *api)
{
    create_entries = api->create_route_entries;
    remove_entries = api->remove_route_entries;
    set_entries_attribute = api->set_route_entries_attribute;
}


template <>
RouteBulker<sai_fdb_entry_t>::RouteBulker(saitraits<sai_fdb_entry_t>::api_t *api)
{
    // TODO: implement after create_fdb_entries() is available in SAI
    throw std::logic_error("Not implemented");
    /*
    create_entries = api->create_fdb_entries;
    remove_entries = api->remove_fdb_entries;
    set_entries_attribute = api->set_fdb_entries_attribute;
    */
}

template <typename T, typename Ts = saitraits<T>>
class NextHopGroupBulker
{
public:
    struct object_entry
    {
        sai_object_id_t *object_id;
        vector<sai_attribute_t> attrs;
    };
    
    NextHopGroupBulker(typename Ts::api_t* next_hop_group_api, sai_object_id_t switch_id)
    {
        throw std::logic_error("Not implemented");
    }
    
    sai_status_t create_entry(
        _Out_ sai_object_id_t *object_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        creating_entries.emplace_back(std::piecewise_construct,
                std::forward_as_tuple(object_id),
                std::forward_as_tuple(attr_list, attr_list + attr_count));
        SWSS_LOG_DEBUG("bulk.create_entry %zu, %zu, %d, %d\n", creating_entries.size(), creating_entries.back().size(), (int)creating_entries.back()[0].id);
        
        *object_id = SAI_NULL_OBJECT_ID; // not created immediately, postponed until flush
        return SAI_STATUS_SUCCESS;
    }

    sai_status_t remove_entry(
        _In_ sai_object_id_t object_id)
    {
        assert(object_id != SAI_NULL_OBJECT_ID);
        if (object_id == SAI_NULL_OBJECT_ID) throw std::invalid_argument("object_id is null");

        auto found_setting = setting_entries.find(object_id);
        if (found_setting != setting_entries.end())
        {
            setting_entries.erase(found_setting);
        }

        removing_entries.push_back(object_id);
        return SAI_STATUS_SUCCESS;
    }

    // TODO: wait until available in SAI
    /*
    sai_status_t set_entry_attribute(
        _In_ sai_object_id_t object_id,
        _In_ const sai_attribute_t *attr)
    {
        auto found_setting = setting_entries.find(object_id);
        if (found_setting != setting_entries.end())
        {
            // For simplicity, just insert new attribute at the vector end, no merging
            found_setting->second.emplace_back(*attr);
        }
        else
        {
            // Create a new key if not exists in the map
            setting_entries.emplace(std::piecewise_construct,
                std::forward_as_tuple(object_id),
                std::forward_as_tuple(1, *attr));
        }

        return SAI_STATUS_SUCCESS;
    }
    */

    void flush()
    {
        // Removing
        if (!removing_entries.empty())
        {
            uint32_t count = (uint32_t)removing_entries.size();
            vector<sai_status_t> statuses(count);
            (*remove_entries)(removing_entries.data(), SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, statuses.data());
            
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
            uint32_t count = (uint32_t)creating_entries.size();
            vector<sai_status_t> statuses(count);
            (*create_entries)(switch_id, count, rs.data(), cs.data(), tss.data()
                , SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, statuses.data());

            SWSS_LOG_NOTICE("bulk.flush creating_entries %zu\n", creating_entries.size());

            creating_entries.clear();
        }

        // Setting
        // TODO: wait until available in SAI
        /*
        if (!setting_entries.empty())
        {
            vector<sai_object_id_t> rs;
            vector<sai_attribute_t> ts;

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
            uint32_t count = (uint32_t)setting_entries.size();
            vector<sai_status_t> statuses(count);
            (*set_entries_attribute)(count, rs.data(), ts.data()
                , SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, statuses.data());

            SWSS_LOG_NOTICE("bulk.flush setting_entries %zu\n", setting_entries.size());

            setting_entries.clear();
        }
        */
    }

    void clear()
    {
        removing_entries.clear();
        creating_entries.clear();
        setting_entries.clear();
    }

private:
    sai_object_id_t                                         switch_id;

    std::vector<object_entry>                           creating_entries;
    std::unordered_map<sai_object_id_t, std::vector<sai_attribute_t>>
                                                            setting_entries;
    std::vector<sai_object_id_t>                            removing_entries;
    
    typename Ts::bulk_create_entry_fn                       create_entries;
    typename Ts::bulk_remove_entry_fn                       remove_entries;
    // TODO: wait until available in SAI
    //typename Ts::bulk_set_entry_attribute_fn                set_entries_attribute;
};

template <>
NextHopGroupBulker<sai_next_hop_group_api_t>::NextHopGroupBulker(saitraits<sai_next_hop_group_api_t>::api_t *api, sai_object_id_t switch_id)
    : switch_id(switch_id)
{
    create_entries = api->create_next_hop_group_members;
    remove_entries = api->remove_next_hop_group_members;
    // TODO: wait until available in SAI
    //set_entries_attribute = ;
}
