#pragma once

#include <assert.h>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <boost/functional/hash.hpp>
#include <sairedis.h>
#include "sai.h"
#include "logger.h"
#include "sai_serialize.h"

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

static inline bool operator==(const sai_inseg_entry_t& a, const sai_inseg_entry_t& b)
{
    return a.switch_id == b.switch_id
        && a.label == b.label
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

    template <>
    struct hash<sai_inseg_entry_t>
    {
        size_t operator()(const sai_inseg_entry_t& a) const noexcept
        {
            size_t seed = 0;
            boost::hash_combine(seed, a.switch_id);
            boost::hash_combine(seed, a.label);
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
struct SaiBulkerTraits { };

template<>
struct SaiBulkerTraits<sai_route_api_t>
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
struct SaiBulkerTraits<sai_fdb_api_t>
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
struct SaiBulkerTraits<sai_next_hop_group_api_t>
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

template<>
struct SaiBulkerTraits<sai_mpls_api_t>
{
    using entry_t = sai_inseg_entry_t;
    using api_t = sai_mpls_api_t;
    using create_entry_fn = sai_create_inseg_entry_fn;
    using remove_entry_fn = sai_remove_inseg_entry_fn;
    using set_entry_attribute_fn = sai_set_inseg_entry_attribute_fn;
    using bulk_create_entry_fn = sai_bulk_create_inseg_entry_fn;
    using bulk_remove_entry_fn = sai_bulk_remove_inseg_entry_fn;
    using bulk_set_entry_attribute_fn = sai_bulk_set_inseg_entry_attribute_fn;
};

template <typename T>
class EntityBulker
{
public:
    using Ts = SaiBulkerTraits<T>;
    using Te = typename Ts::entry_t;

    EntityBulker(typename Ts::api_t *api, size_t max_bulk_size) :
        max_bulk_size(max_bulk_size)
    {
        throw std::logic_error("Not implemented");
    }

    sai_status_t create_entry(
        _Out_ sai_status_t *object_status,
        _In_ const Te *entry,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        assert(object_status);
        if (!object_status) throw std::invalid_argument("object_status is null");
        assert(entry);
        if (!entry) throw std::invalid_argument("entry is null");
        assert(attr_list);
        if (!attr_list) throw std::invalid_argument("attr_list is null");

        auto rc = creating_entries.emplace(std::piecewise_construct,
                std::forward_as_tuple(*entry),
                std::forward_as_tuple());
        auto it = rc.first;
        bool inserted = rc.second;
        if (!inserted)
        {
            SWSS_LOG_INFO("EntityBulker.create_entry not inserted %zu\n", creating_entries.size());
            *object_status = SAI_STATUS_ITEM_ALREADY_EXISTS;
            return *object_status;
        }

        auto& attrs = it->second.first;
        attrs.insert(attrs.end(), attr_list, attr_list + attr_count);
        it->second.second = object_status;
        SWSS_LOG_INFO("EntityBulker.create_entry %zu, %zu, %d, %d\n", creating_entries.size(), it->second.first.size(), (int)it->second.first[0].id, inserted);
        *object_status = SAI_STATUS_NOT_EXECUTED;
        return *object_status;
    }

    sai_status_t remove_entry(
        _Out_ sai_status_t *object_status,
        _In_ const Te *entry)
    {
        assert(object_status);
        if (!object_status) throw std::invalid_argument("object_status is null");
        assert(entry);
        if (!entry) throw std::invalid_argument("entry is null");

        auto found_setting = setting_entries.find(*entry);
        if (found_setting != setting_entries.end())
        {
            // Mark old one as done
            auto& attrs = found_setting->second;
            for (auto& attr: attrs)
            {
                *attr.second = SAI_STATUS_SUCCESS;
            }
            // Erase old one
            setting_entries.erase(found_setting);
        }

        auto found_creating = creating_entries.find(*entry);
        if (found_creating != creating_entries.end())
        {
            // Mark old ones as done
            *found_creating->second.second = SAI_STATUS_SUCCESS;
            // Erase old one
            creating_entries.erase(found_creating);
            // No need to keep in bulker, claim success immediately
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
        assert(object_status);
        if (!object_status) throw std::invalid_argument("object_status is null");
        assert(entry);
        if (!entry) throw std::invalid_argument("entry is null");
        assert(attr);
        if (!attr) throw std::invalid_argument("attr is null");

        // Insert or find the key (entry)
        auto& attrs = setting_entries.emplace(std::piecewise_construct,
                std::forward_as_tuple(*entry),
                std::forward_as_tuple()
        ).first->second;

        // Insert attr
        attrs.emplace_back(std::piecewise_construct,
                std::forward_as_tuple(*attr),
                std::forward_as_tuple(object_status));
        *object_status = SAI_STATUS_NOT_EXECUTED;
    }

    void flush()
    {
        // Removing
        if (!removing_entries.empty())
        {
            std::vector<Te> rs;

            for (auto& i: removing_entries)
            {
                auto const& entry = i.first;
                sai_status_t *object_status = i.second;
                if (*object_status == SAI_STATUS_NOT_EXECUTED)
                {
                    rs.push_back(entry);

                    if (rs.size() >= max_bulk_size)
                    {
                        flush_removing_entries(rs);
                    }
                }
            }
            flush_removing_entries(rs);

            removing_entries.clear();
        }

        // Creating
        if (!creating_entries.empty())
        {
            std::vector<Te> rs;
            std::vector<sai_attribute_t const*> tss;
            std::vector<uint32_t> cs;

            for (auto const& i: creating_entries)
            {
                auto const& entry = i.first;
                auto const& attrs = i.second.first;
                sai_status_t *object_status = i.second.second;
                if (*object_status == SAI_STATUS_NOT_EXECUTED)
                {
                    rs.push_back(entry);
                    tss.push_back(attrs.data());
                    cs.push_back((uint32_t)attrs.size());

                    if (rs.size() >= max_bulk_size)
                    {
                        flush_creating_entries(rs, tss, cs);
                    }
                }
            }
            flush_creating_entries(rs, tss, cs);

            creating_entries.clear();
        }

        // Setting
        if (!setting_entries.empty())
        {
            std::vector<Te> rs;
            std::vector<sai_attribute_t> ts;
            std::vector<sai_status_t*> status_vector;

            for (auto const& i: setting_entries)
            {
                auto const& entry = i.first;
                auto const& attrs = i.second;
                for (auto const& ia: attrs)
                {
                    auto const& attr = ia.first;
                    sai_status_t *object_status = ia.second;
                    if (*object_status == SAI_STATUS_NOT_EXECUTED)
                    {
                        rs.push_back(entry);
                        ts.push_back(attr);
                        status_vector.push_back(object_status);

                        if (rs.size() >= max_bulk_size)
                        {
                            flush_setting_entries(rs, ts, status_vector);
                        }
                    }
                }
            }
            flush_setting_entries(rs, ts, status_vector);

            setting_entries.clear();
        }
    }

    void clear()
    {
        removing_entries.clear();
        creating_entries.clear();
        setting_entries.clear();
    }

    size_t creating_entries_count() const
    {
        return creating_entries.size();
    }

    size_t setting_entries_count() const
    {
        return setting_entries.size();
    }

    size_t removing_entries_count() const
    {
        return removing_entries.size();
    }

    size_t creating_entries_count(const Te& entry) const
    {
        return creating_entries.count(entry);
    }

    bool bulk_entry_pending_removal(const Te& entry) const
    {
        return removing_entries.find(entry) != removing_entries.end();
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
            std::vector<                                    //     vector of attribute and status
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

    size_t max_bulk_size;

    typename Ts::bulk_create_entry_fn                       create_entries;
    typename Ts::bulk_remove_entry_fn                       remove_entries;
    typename Ts::bulk_set_entry_attribute_fn                set_entries_attribute;

    sai_status_t flush_removing_entries(
        _Inout_ std::vector<Te> &rs)
    {
        if (rs.empty())
        {
            return SAI_STATUS_SUCCESS;
        }
        size_t count = rs.size();
        std::vector<sai_status_t> statuses(count);
        sai_status_t status = (*remove_entries)((uint32_t)count, rs.data(), SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());
        if (status == SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_INFO("EntityBulker.flush removing_entries %zu\n", count);
        }
        else
        {
            SWSS_LOG_ERROR("EntityBulker.flush remove entries failed, number of entries to remove: %zu, status: %s",
                            count, sai_serialize_status(status).c_str());
        }

        for (size_t ir = 0; ir < count; ir++)
        {
            auto& entry = rs[ir];
            sai_status_t *object_status = removing_entries[entry];
            if (object_status)
            {
                *object_status = statuses[ir];
            }
        }

        rs.clear();

        return status;
    }

    sai_status_t flush_creating_entries(
        _Inout_ std::vector<Te> &rs,
        _Inout_ std::vector<sai_attribute_t const*> &tss,
        _Inout_ std::vector<uint32_t> &cs)
    {
        if (rs.empty())
        {
            return SAI_STATUS_SUCCESS;
        }
        size_t count = rs.size();
        std::vector<sai_status_t> statuses(count);
        sai_status_t status = (*create_entries)((uint32_t)count, rs.data(), cs.data(), tss.data()
            , SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());
        if (status == SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_INFO("EntityBulker.flush creating_entries %zu\n", count);
        }
        else
        {
            SWSS_LOG_ERROR("EntityBulker.flush create entries failed, number of entries to create: %zu, status: %s",
                            count, sai_serialize_status(status).c_str());
        }

        for (size_t ir = 0; ir < count; ir++)
        {
            auto& entry = rs[ir];
            sai_status_t *object_status = creating_entries[entry].second;
            if (object_status)
            {
                *object_status = statuses[ir];
            }
        }

        rs.clear();
        tss.clear();
        cs.clear();

        return status;
    }

    sai_status_t flush_setting_entries(
        _Inout_ std::vector<Te> &rs,
        _Inout_ std::vector<sai_attribute_t> &ts,
        _Inout_ std::vector<sai_status_t*> &status_vector)
    {
        if (rs.empty())
        {
            return SAI_STATUS_SUCCESS;
        }
        size_t count = rs.size();
        std::vector<sai_status_t> statuses(count);
        sai_status_t status = (*set_entries_attribute)((uint32_t)count, rs.data(), ts.data()
            , SAI_BULK_OP_ERROR_MODE_IGNORE_ERROR, statuses.data());
        if (status == SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_INFO("EntityBulker.flush setting_entries, count %zu\n", count);
        }
        else
        {
            SWSS_LOG_ERROR("EntityBulker.flush set entry attribute failed, number of entries to set: %zu, status: %s",
                            count, sai_serialize_status(status).c_str());
        }

        for (size_t ir = 0; ir < count; ir++)
        {
            sai_status_t *object_status = status_vector[ir];
            if (object_status)
            {
                SWSS_LOG_INFO("EntityBulker.flush setting_entries status[%zu]=%d(0x%8p)\n", ir, statuses[ir], object_status);
                *object_status = statuses[ir];
            }
        }

        rs.clear();
        ts.clear();
        status_vector.clear();

        return status;
    }
};

template <>
inline EntityBulker<sai_route_api_t>::EntityBulker(sai_route_api_t *api, size_t max_bulk_size) :
    max_bulk_size(max_bulk_size)
{
    create_entries = api->create_route_entries;
    remove_entries = api->remove_route_entries;
    set_entries_attribute = api->set_route_entries_attribute;
}

template <>
inline EntityBulker<sai_fdb_api_t>::EntityBulker(sai_fdb_api_t *api, size_t max_bulk_size) :
    max_bulk_size(max_bulk_size)
{
    // TODO: implement after create_fdb_entries() is available in SAI
    throw std::logic_error("Not implemented");
    /*
    create_entries = api->create_fdb_entries;
    remove_entries = api->remove_fdb_entries;
    set_entries_attribute = api->set_fdb_entries_attribute;
    */
}

template <>
inline EntityBulker<sai_mpls_api_t>::EntityBulker(sai_mpls_api_t *api, size_t max_bulk_size) :
    max_bulk_size(max_bulk_size)
{
    create_entries = api->create_inseg_entries;
    remove_entries = api->remove_inseg_entries;
    set_entries_attribute = api->set_inseg_entries_attribute;
}

template <typename T>
class ObjectBulker
{
public:
    using Ts = SaiBulkerTraits<T>;

    ObjectBulker(typename Ts::api_t* next_hop_group_api, sai_object_id_t switch_id, size_t max_bulk_size) :
        max_bulk_size(max_bulk_size)
    {
        throw std::logic_error("Not implemented");
    }

    sai_status_t create_entry(
        _Out_ sai_object_id_t *object_id,
        _In_ uint32_t attr_count,
        _In_ const sai_attribute_t *attr_list)
    {
        assert(object_id);
        if (!object_id) throw std::invalid_argument("object_id is null");
        assert(attr_list);
        if (!attr_list) throw std::invalid_argument("attr_list is null");

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
        assert(object_status);
        if (!object_status) throw std::invalid_argument("object_status is null");
        assert(object_id != SAI_NULL_OBJECT_ID);
        if (object_id == SAI_NULL_OBJECT_ID) throw std::invalid_argument("object_id is null");

        auto found_setting = setting_entries.find(object_id);
        if (found_setting != setting_entries.end())
        {
            setting_entries.erase(found_setting);
        }

        removing_entries.emplace(object_id, object_status);
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
            std::vector<sai_object_id_t> rs;
            for (auto const& i: removing_entries)
            {
                auto const& entry = i.first;
                sai_status_t *object_status = i.second;
                if (*object_status == SAI_STATUS_NOT_EXECUTED)
                {
                    rs.push_back(entry);

                    if (rs.size() >= max_bulk_size)
                    {
                        flush_removing_entries(rs);
                    }
                }
            }
            flush_removing_entries(rs);

            removing_entries.clear();
        }

        // Creating
        if (!creating_entries.empty())
        {
            std::vector<sai_object_id_t *> rs;
            std::vector<sai_attribute_t const*> tss;
            std::vector<uint32_t> cs;

            for (auto const& i: creating_entries)
            {
                sai_object_id_t *pid = std::get<0>(i);
                auto const& attrs = std::get<1>(i);
                if (*pid == SAI_NULL_OBJECT_ID)
                {
                    rs.push_back(pid);
                    tss.push_back(attrs.data());
                    cs.push_back((uint32_t)attrs.size());

                    if (rs.size() >= max_bulk_size)
                    {
                        flush_creating_entries(rs, tss, cs);
                    }
                }
            }
            flush_creating_entries(rs, tss, cs);

            creating_entries.clear();
        }

        // Setting
        // TODO: wait until available in SAI
        /*
        if (!setting_entries.empty())
        {
            std::vector<sai_object_id_t> rs;
            std::vector<sai_attribute_t> ts;

            for (auto const& i: setting_entries)
            {
                auto const& entry = i.first;
                auto const& attrs = i.second;
                for (auto const& attr: attrs)
                {
                    rs.push_back(entry);
                    ts.push_back(attr);

                    if (rs.size() >= max_bulk_size)
                    {
                        flush_setting_entries(rs, ts);
                    }
                }
            }
            flush_setting_entries(rs, ts);

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

    size_t creating_entries_count() const
    {
        return creating_entries.size();
    }

    size_t setting_entries_count() const
    {
        return setting_entries.size();
    }

    size_t removing_entries_count() const
    {
        return removing_entries.size();
    }

private:
    struct object_entry
    {
        sai_object_id_t *object_id;
        std::vector<sai_attribute_t> attrs;
        template <class InputIterator>
        object_entry(sai_object_id_t *object_id, InputIterator first, InputIterator last)
            : object_id(object_id)
            , attrs(first, last)
        {
        }
    };

    sai_object_id_t                                         switch_id;

    size_t max_bulk_size;

    std::vector<std::pair<                                  // A vector of pair of
            sai_object_id_t *,                              // - object_id
            std::vector<sai_attribute_t>                    // - attrs
    >>                                                      creating_entries;

    std::unordered_map<                                     // A map of
            sai_object_id_t,                                // object_id -> (OUT object_status, attributes)
            std::pair<
                    sai_status_t *,
                    std::vector<sai_attribute_t>
            >
    >                                                       setting_entries;

                                                            // A map of
                                                            // object_id -> object_status
    std::unordered_map<sai_object_id_t, sai_status_t *>     removing_entries;

    typename Ts::bulk_create_entry_fn                       create_entries;
    typename Ts::bulk_remove_entry_fn                       remove_entries;
    // TODO: wait until available in SAI
    //typename Ts::bulk_set_entry_attribute_fn                set_entries_attribute;

    sai_status_t flush_removing_entries(
        _Inout_ std::vector<sai_object_id_t> &rs)
    {
        if (rs.empty())
        {
            return SAI_STATUS_SUCCESS;
        }
        size_t count = rs.size();
        std::vector<sai_status_t> statuses(count);
        sai_status_t status = (*remove_entries)((uint32_t)count, rs.data(), SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, statuses.data());
        if (status == SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_INFO("ObjectBulker.flush removing_entries %zu rc=%d statuses[0]=%d\n", removing_entries.size(), status, statuses[0]);
        }
        else
        {
            SWSS_LOG_ERROR("ObjectBulker.flush remove entries failed, number of entries to remove: %zu, status: %s",
                            removing_entries.size(), sai_serialize_status(status).c_str());
        }

        for (size_t i = 0; i < count; i++)
        {
            auto const& entry = rs[i];
            sai_status_t object_status = statuses[i];
            *removing_entries[entry] = object_status;
        }

        rs.clear();

        return status;
    }

    sai_status_t flush_creating_entries(
        _Inout_ std::vector<sai_object_id_t *> &rs,
        _Inout_ std::vector<sai_attribute_t const*> &tss,
        _Inout_ std::vector<uint32_t> &cs)
    {
        if (rs.empty())
        {
            return SAI_STATUS_SUCCESS;
        }
        size_t count = rs.size();
        std::vector<sai_object_id_t> object_ids(count);
        std::vector<sai_status_t> statuses(count);
        sai_status_t status = (*create_entries)(switch_id, (uint32_t)count, cs.data(), tss.data()
            , SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, object_ids.data(), statuses.data());
        if (status == SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_INFO("ObjectBulker.flush creating_entries %zu\n", count);
        }
        else
        {
            SWSS_LOG_ERROR("ObjectBulker.flush create entries failed, number of entries to create: %zu, status: %s",
                            count, sai_serialize_status(status).c_str());
        }

        for (size_t i = 0; i < count; i++)
        {
            sai_object_id_t *pid = rs[i];
            *pid = (statuses[i] == SAI_STATUS_SUCCESS) ? object_ids[i] : SAI_NULL_OBJECT_ID;
        }

        rs.clear();
        tss.clear();
        cs.clear();

        return status;
    }

    // TODO: wait until available in SAI
    /*
    sai_status_t flush_setting_entries(
        _Inout_ std::vector<sai_object_id_t> &rs,
        _Inout_ std::vector<sai_attribute_t> &ts)
    {
        if (rs.empty())
        {
            return SAI_STATUS_SUCCESS;
        }
        size_t count = rs.size();
        std::vector<sai_status_t> statuses(count);
        sai_status_t status = (*set_entries_attribute)((uint32_t)count, rs.data(), ts.data()
            , SAI_BULK_OP_ERROR_MODE_STOP_ON_ERROR, statuses.data());
        if (status == SAI_STATUS_SUCCESS)
        {
            SWSS_LOG_INFO("ObjectBulker.flush setting_entries %zu\n", count);
        }
        else
        {
            SWSS_LOG_ERROR("ObjectBulker.flush set entry attribute failed, number of entries to set: %zu, status: %s",
                            count, sai_serialize_status(status).c_str());
        }

        rs.clear();
        ts.clear();

        return status;
    }
     */
};

template <>
inline ObjectBulker<sai_next_hop_group_api_t>::ObjectBulker(SaiBulkerTraits<sai_next_hop_group_api_t>::api_t *api, sai_object_id_t switch_id, size_t max_bulk_size) : 
    switch_id(switch_id),
    max_bulk_size(max_bulk_size)
{
    create_entries = api->create_next_hop_group_members;
    remove_entries = api->remove_next_hop_group_members;
    // TODO: wait until available in SAI
    //set_entries_attribute = ;
}
