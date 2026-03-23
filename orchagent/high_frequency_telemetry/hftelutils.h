#pragma once

#include <sai.h>
#include <saitypes.h>

#include <set>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

class HFTelUtils
{
public:
    HFTelUtils() = delete;

    static std::vector<sai_object_id_t> get_sai_object_list(
        sai_object_id_t obj,
        sai_attr_id_t attr_id,
        sai_api_t api,
        std::function<sai_status_t(sai_object_id_t, uint32_t, sai_attribute_t*)> get_attribute_handler);
    static sai_object_type_t group_name_to_sai_type(const std::string &group_name);
    static std::string sai_type_to_group_name(sai_object_type_t object_type);
    static std::set<sai_stat_id_t> object_counters_to_stats_ids(
        const std::string &group_name,
        const std::set<std::string> &object_counters);
    static sai_stats_mode_t get_stats_mode(sai_object_type_t object_type, sai_stat_id_t stat_id);
};

#define HFTELUTILS_ADD_SAI_OBJECT_LIST(obj, attr_id, inserted_obj, api_type_name, api_name, obj_type_name) \
    {                                                                                                      \
        sai_attribute_t attr;                                                                              \
        auto obj_list = HFTelUtils::get_sai_object_list(                                                   \
            obj,                                                                                           \
            attr_id,                                                                                       \
            api_type_name,                                                                                 \
            sai_##api_name##_api->get_##obj_type_name##_attribute);                                        \
        obj_list.push_back(inserted_obj);                                                                  \
        attr.id = attr_id;                                                                                 \
        attr.value.objlist.count = static_cast<uint32_t>(obj_list.size());                                 \
        attr.value.objlist.list = obj_list.data();                                                         \
        sai_status_t status = sai_##api_name##_api->set_##obj_type_name##_attribute(                       \
            obj,                                                                                           \
            &attr);                                                                                        \
        if (status != SAI_STATUS_SUCCESS)                                                                  \
        {                                                                                                  \
            handleSaiSetStatus(                                                                            \
                api_type_name,                                                                             \
                status);                                                                                   \
        }                                                                                                  \
    }

#define HFTELUTILS_DEL_SAI_OBJECT_LIST(obj, attr_id, removed_obj, api_type_name, api_name, obj_type_name) \
    {                                                                                                     \
        sai_attribute_t attr;                                                                             \
        auto obj_list = HFTelUtils::get_sai_object_list(                                                  \
            obj,                                                                                          \
            attr_id,                                                                                      \
            api_type_name,                                                                                \
            sai_##api_name##_api->get_##obj_type_name##_attribute);                                       \
        obj_list.erase(                                                                                   \
            std::remove(                                                                                  \
                obj_list.begin(),                                                                         \
                obj_list.end(),                                                                           \
                removed_obj),                                                                             \
            obj_list.end());                                                                              \
        attr.id = attr_id;                                                                                \
        attr.value.objlist.count = static_cast<uint32_t>(obj_list.size());                                \
        attr.value.objlist.list = obj_list.data();                                                        \
        sai_status_t status = sai_##api_name##_api->set_##obj_type_name##_attribute(                      \
            obj,                                                                                          \
            &attr);                                                                                       \
        if (status != SAI_STATUS_SUCCESS)                                                                 \
        {                                                                                                 \
            handleSaiSetStatus(                                                                           \
                api_type_name,                                                                            \
                status);                                                                                  \
        }                                                                                                 \
    }
