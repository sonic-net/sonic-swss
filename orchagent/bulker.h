#pragma once

#include <assert.h>
#include <deque>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <boost/functional/hash.hpp>
#include <sairedis.h>
#include "sai.h"

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
struct saitraits<sai_route_api_t>
{
    using entry_t = sai_route_entry_t;
    using api_t = sai_route_api_t;
    using create_entry_fn = sai_create_route_entry_fn;
    using remove_entry_fn = sai_remove_route_entry_fn;
    using set_entry_attribute_fn = sai_set_route_entry_attribute_fn;
    using bulk_create_entry_fn = sai_bulk_create_route_entry_fn;
    using bulk_remove_entry_fn = sai_bulk_remove_route_entry_fn;
    using bulk_set_entry_attribute_fn = sai_bulk_set_route_entry_attribute_fn;
};

template<>
struct saitraits<sai_fdb_api_t>
{
    using entry_t = sai_fdb_entry_t;
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
    using entry_t = sai_object_id_t;
    using api_t = sai_next_hop_group_api_t;
    using create_entry_fn = sai_create_next_hop_group_member_fn;
    using remove_entry_fn = sai_remove_next_hop_group_member_fn;
    using set_entry_attribute_fn = sai_set_next_hop_group_member_attribute_fn;
    using bulk_create_entry_fn = sai_bulk_object_create_fn;
    using bulk_remove_entry_fn = sai_bulk_object_remove_fn;
    // TODO: wait until available in SAI
    //using bulk_set_entry_attribute_fn = sai_bulk_object_set_attribute_fn;
};

template <typename T>
class EntityBulker
{
public:
    using Ts = saitraits<T>;
    using Te = typename Ts::entry_t;

    EntityBulker(typename Ts::api_t *api)
    {
        throw std::logic_error("Not implemented");
    }

    sai_status_t create_entry(
        _Out_ sai_status_t *object_status,
        _In_ const Te *entry,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        auto rc = creating_entries.emplace(std::piecewise_construct,
                std::forward_as_tuple(*entry),
                std::forward_as_tuple());
        auto it = rc.first;
        bool inserted = rc.second;
        if (!inserted)
        {
            SWSS_LOG_DEBUG("bulk.create_entry not inserted %zu\n", creating_entries.size());
            *object_status = SAI_STATUS_ITEM_ALREADY_EXISTS;
            return *object_status;
        }

        auto& attrs = it->second.first;
        attrs.insert(attrs.end(), attr_list, attr_list + attr_count);
        it->second.second = object_status;
        SWSS_LOG_DEBUG("bulk.create_entry %zu, %zu, %d, %d\n", creating_entries.size(), it->second.first.size(), (int)it->second.first[0].id, inserted);
        *object_status = SAI_STATUS_NOT_EXECUTED;
        return *object_status;
    }

    sai_status_t remove_entry(
        _Out_ sai_status_t *object_status,
        _In_ const Te *entry)
    {
        assert(entry);
        if (!entry) throw std::invalid_argument("entry is null");

        auto found_setting = setting_entries.find(*entry);
        if (found_setting != setting_entries.end())
        {
            // TODO: mark old ones as done
            setting_entries.erase(found_setting);
        }

        auto found_creating = creating_entries.find(*entry);
        if (found_creating != creating_entries.end())
        {
            // TODO: mark old ones as done
            creating_entries.erase(found_creating);
            *object_status = SAI_STATUS_SUCCESS;
            SWSS_LOG_INFO("EntityBulker.remove_entry quickly removed %zu, creating_entries.size=%zu\n", removing_entries.size(), creating_entries.size());
            return *object_status;
        }
        auto rc = removing_entries.emplace(std::piecewise_construct,
                std::forward_as_tuple(*entry),
                std::forward_as_tuple(object_status));
        bool inserted = rc.second;
        SWSS_LOG_INFO("EntityBulker.remove_entry %zu, %d\n", removing_entries.size(), inserted);

        *object_status = SAI_STATUS_NOT_EXECUTED;
        return *object_status;
    }

    void set_entry_attribute(
        _Out_ sai_status_t *object_status,
        _In_ const Te *entry,
        _In_ const sai_attribute_t *attr)
    {
        // Insert or find the key (entry)
        auto& attrmap = setting_entries.emplace(std::piecewise_construct,
                std::forward_as_tuple(*entry),
                std::forward_as_tuple()
        ).first->second;

        // Insert or find the key (attr)
        auto rc = attrmap.emplace(std::piecewise_construct,
                std::forward_as_tuple(attr->id),
                std::forward_as_tuple());
        bool inserted = rc.second;
        auto it = rc.first;

        // If inserted new key, assign the attr
        // If found existing key, overwrite the old attr
        it->second.first = *attr;
        if (!inserted)
        {
            // If found existing key, mark old status as success
            *it->second.second = SAI_STATUS_SUCCESS;
        }
        it->second.second = object_status;
        *object_status = SAI_STATUS_NOT_EXECUTED;
    }

    void flush()
    {
        // Removing
        if (!removing_entries.empty())
        {
            vector<Te> rs;

            for (auto& i: removing_entries)
            {
                auto& entry = i.first;
                rs.push_back(entry);
            }
            size_t count = rs.size();
            vector<sai_status_t> statuses(count);
            (*remove_entries)((uint32_t)count, rs.data(), SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());
            SWSS_LOG_NOTICE("EntityBulker.flush removing_entries %zu\n", removing_entries.size());

            for (size_t ir = 0; ir < count; ir++)
            {
                auto& entry = rs[ir];
                sai_status_t *object_status = removing_entries[entry];
                if (object_status)
                {
                    *object_status = statuses[ir];
                }
            }
            removing_entries.clear();
        }

        // Creating
        if (!creating_entries.empty())
        {
            vector<Te> rs;
            vector<sai_attribute_t const*> tss;
            vector<uint32_t> cs;

            for (auto const& i: creating_entries)
            {
                auto const& entry = i.first;
                auto const& attrs = i.second.first;

                rs.push_back(entry);
                tss.push_back(attrs.data());
                cs.push_back((uint32_t)attrs.size());
            }
            size_t count = rs.size();
            vector<sai_status_t> statuses(count);
            (*create_entries)((uint32_t)count, rs.data(), cs.data(), tss.data()
                , SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());
            SWSS_LOG_NOTICE("EntityBulker.flush creating_entries %zu\n", creating_entries.size());

            for (size_t ir = 0; ir < count; ir++)
            {
                auto& entry = rs[ir];
                sai_status_t *object_status = creating_entries[entry].second;
                if (object_status)
                {
                    *object_status = statuses[ir];
                }
            }
            creating_entries.clear();
        }

        // Setting
        if (!setting_entries.empty())
        {
            vector<Te> rs;
            vector<sai_attribute_t> ts;

            for (auto const& i: setting_entries)
            {
                auto const& entry = i.first;
                auto const& attrmap = i.second;
                for (auto const& ia: attrmap)
                {
                    rs.push_back(entry);
                    ts.push_back(ia.second.first);
                }
            }
            size_t count = rs.size();
            vector<sai_status_t> statuses(count);
            (*set_entries_attribute)((uint32_t)count, rs.data(), ts.data()
                , SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());
            SWSS_LOG_NOTICE("EntityBulker.flush setting_entries %zu, count %zu\n", setting_entries.size(), count);

            for (size_t ir = 0; ir < count; ir++)
            {
                auto& entry = rs[ir];
                auto& attr_id = ts[ir].id;
                sai_status_t *object_status = setting_entries[entry][attr_id].second;
                if (object_status)
                {
                    *object_status = statuses[ir];
                }
            }
            setting_entries.clear();
        }
    }

    void clear()
    {
        removing_entries.clear();
        creating_entries.clear();
        setting_entries.clear();
    }

    size_t creating_entries_count()
    {
        return creating_entries.size();
    }

    size_t setting_entries_count()
    {
        return setting_entries.size();
    }

    size_t removing_entries_count()
    {
        return removing_entries.size();
    }

private:
    std::unordered_map<                                     // A map of
            Te,                                             // entry ->
            std::pair<
                    std::vector<sai_attribute_t>,           // (attributes, OUT object_status)
                    sai_status_t *
            >
    >                                                       creating_entries;

    std::unordered_map<                                     // A map of
            Te,                                             // entry ->
            std::unordered_map<                             //     another map of
                    sai_attr_id_t,                          //     attr_id ->
                    std::pair<
                            sai_attribute_t,                //     (attr_value, OUT object_status)
                            sai_status_t *
                    >
            >
    >                                                       setting_entries;

    std::unordered_map<                                     // A map of
            Te,                                             // entry ->
            sai_status_t *                                  // OUT object_status
    >                                                       removing_entries;

    typename Ts::bulk_create_entry_fn                       create_entries;
    typename Ts::bulk_remove_entry_fn                       remove_entries;
    typename Ts::bulk_set_entry_attribute_fn                set_entries_attribute;
};

template <>
inline EntityBulker<sai_route_api_t>::EntityBulker(sai_route_api_t *api)
{
    create_entries = api->create_route_entries;
    remove_entries = api->remove_route_entries;
    set_entries_attribute = api->set_route_entries_attribute;
}

template <>
inline EntityBulker<sai_fdb_api_t>::EntityBulker(sai_fdb_api_t *api)
{
    // TODO: implement after create_fdb_entries() is available in SAI
    throw std::logic_error("Not implemented");
    /*
    create_entries = api->create_fdb_entries;
    remove_entries = api->remove_fdb_entries;
    set_entries_attribute = api->set_fdb_entries_attribute;
    */
}

template <typename T>
class ObjectBulker
{
public:
    using Ts = saitraits<T>;

    ObjectBulker(typename Ts::api_t* next_hop_group_api, sai_object_id_t switch_id)
    {
        throw std::logic_error("Not implemented");
    }

    sai_status_t create_entry(
        _Out_ sai_object_id_t *object_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        creating_entries.emplace_back(std::piecewise_construct, std::forward_as_tuple(object_id), std::forward_as_tuple(attr_list, attr_list + attr_count));

        auto& last_attrs = std::get<1>(creating_entries.back());
        SWSS_LOG_INFO("ObjectBulker.create_entry %zu, %zu, %u\n", creating_entries.size(), last_attrs.size(), last_attrs[0].id);

        *object_id = SAI_NULL_OBJECT_ID; // not created immediately, postponed until flush
        return SAI_STATUS_NOT_EXECUTED;
    }

    sai_status_t remove_entry(
        _Out_ sai_status_t *object_status,
        _In_ sai_object_id_t object_id)
    {
        assert(object_id != SAI_NULL_OBJECT_ID);
        if (object_id == SAI_NULL_OBJECT_ID) throw std::invalid_argument("object_id is null");

        auto found_setting = setting_entries.find(object_id);
        if (found_setting != setting_entries.end())
        {
            setting_entries.erase(found_setting);
        }

        removing_entries.emplace_back(object_id);
        removing_statuses.emplace_back(object_status);
        *object_status = SAI_STATUS_NOT_EXECUTED;
        return *object_status;
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
            size_t count = removing_entries.size();
            vector<sai_status_t> statuses(count);
            sai_status_t status = (*remove_entries)((uint32_t)count, removing_entries.data(), SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, statuses.data());
            SWSS_LOG_NOTICE("ObjectBulker.flush removing_entries %zu rc=%d statuses[0]=%d\n", removing_entries.size(), status, statuses[0]);

            for (size_t i = 0; i < count; i++)
            {
                sai_status_t *object_status = removing_statuses[i];
                if (object_status)
                {
                    *object_status = statuses[i];
                }
            }
            removing_entries.clear();
            removing_statuses.clear();
        }

        // Creating
        if (!creating_entries.empty())
        {
            vector<sai_attribute_t const*> tss;
            vector<uint32_t> cs;

            for (auto const& i: creating_entries)
            {
                auto const& attrs = std::get<1>(i);
                tss.push_back(attrs.data());
                cs.push_back((uint32_t)attrs.size());
            }
            size_t count = creating_entries.size();
            vector<sai_object_id_t> object_ids(count);
            vector<sai_status_t> statuses(count);
            (*create_entries)(switch_id, (uint32_t)count, cs.data(), tss.data()
                , SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, object_ids.data(), statuses.data());
            SWSS_LOG_NOTICE("ObjectBulker.flush creating_entries %zu\n", creating_entries.size());

            for (size_t i = 0; i < count; i++)
            {
                sai_object_id_t *pid = std::get<0>(creating_entries[i]);
                *pid = (statuses[i] == SAI_STATUS_SUCCESS) ? object_ids[i] : SAI_NULL_OBJECT_ID;
            }

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
                auto const& entry = i.first;
                auto const& attrs = i.second;
                for (auto const& attr: attrs)
                {
                    rs.push_back(entry);
                    ts.push_back(attr);
                }
            }
            size_t count = setting_entries.size();
            vector<sai_status_t> statuses(count);
            (*set_entries_attribute)((uint32_t)count, rs.data(), ts.data()
                , SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, statuses.data());

            SWSS_LOG_NOTICE("ObjectBulker.flush setting_entries %zu\n", setting_entries.size());

            setting_entries.clear();
        }
        */
    }

    void clear()
    {
        removing_entries.clear();
        removing_statuses.clear();
        creating_entries.clear();
        setting_entries.clear();
    }

    size_t creating_entries_count()
    {
        return creating_entries.size();
    }

    size_t setting_entries_count()
    {
        return setting_entries.size();
    }

    size_t removing_entries_count()
    {
        return removing_entries.size();
    }

private:
    struct object_entry
    {
        sai_object_id_t *object_id;
        vector<sai_attribute_t> attrs;
        template <class InputIterator>
        object_entry(sai_object_id_t *object_id, InputIterator first, InputIterator last)
            : object_id(object_id)
            , attrs(first, last)
        {
        }
    };

    sai_object_id_t                                         switch_id;

    std::vector<std::pair<                                  // A vector of pair of
            sai_object_id_t *,                              // - object_id
            vector<sai_attribute_t>                         // - attrs
    >>                                                      creating_entries;

    std::unordered_map<                                     // A map of
            sai_object_id_t,                                // object_id -> (OUT object_status, attributes)
            std::pair<
                    sai_status_t *,
                    std::vector<sai_attribute_t>
            >
    >                                                       setting_entries;

    std::vector<sai_object_id_t>                            removing_entries;
    std::vector<sai_status_t *>                             removing_statuses;

    typename Ts::bulk_create_entry_fn                       create_entries;
    typename Ts::bulk_remove_entry_fn                       remove_entries;
    // TODO: wait until available in SAI
    //typename Ts::bulk_set_entry_attribute_fn                set_entries_attribute;
};

template <>
inline ObjectBulker<sai_next_hop_group_api_t>::ObjectBulker(saitraits<sai_next_hop_group_api_t>::api_t *api, sai_object_id_t switch_id)
    : switch_id(switch_id)
{
    create_entries = api->create_next_hop_group_members;
    remove_entries = api->remove_next_hop_group_members;
    // TODO: wait until available in SAI
    //set_entries_attribute = ;
}
