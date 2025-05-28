#include "hftelutils.h"

#include <saihelper.h>
#include <swss/logger.h>

#include <boost/algorithm/string.hpp>
#include <regex>

using namespace std;

#define OBJECT_TYPE_PREFIX "SAI_OBJECT_TYPE_"

vector<sai_object_id_t> HFTelUtils::get_sai_object_list(
    sai_object_id_t obj,
    sai_attr_id_t attr_id,
    sai_api_t api,
    function<sai_status_t(sai_object_id_t, uint32_t, sai_attribute_t*)> get_attribute_handler)
{
    SWSS_LOG_ENTER();

    vector<sai_object_id_t> obj_list(1024, SAI_NULL_OBJECT_ID);
    sai_attribute_t attr;

    attr.id = attr_id;
    attr.value.objlist.count = static_cast<uint32_t>(obj_list.size());
    attr.value.objlist.list = obj_list.data();

    auto status = get_attribute_handler(
        obj,
        1,
        &attr);
    if (status != SAI_STATUS_SUCCESS)
    {
        handleSaiGetStatus(
            api,
            status);
    }
    assert(attr.value.objlist.count < obj_list.size());

    obj_list.erase(
        obj_list.begin() + attr.value.objlist.count,
        obj_list.end());

    return obj_list;
}

sai_object_type_t HFTelUtils::group_name_to_sai_type(const string &group_name)
{
    SWSS_LOG_ENTER();

    sai_object_type_t sai_object_type;

    sai_deserialize_object_type(string(OBJECT_TYPE_PREFIX) + boost::to_upper_copy(group_name), sai_object_type);
    return sai_object_type;
}

std::string HFTelUtils::sai_type_to_group_name(sai_object_type_t object_type)
{
    SWSS_LOG_ENTER();

    std::string group_name = sai_serialize_object_type(object_type);

    group_name.erase(0, sizeof(OBJECT_TYPE_PREFIX) - 1);

    return group_name;
}

set<sai_stat_id_t> HFTelUtils::object_counters_to_stats_ids(
    const string &group_name,
    const set<string> &object_counters)
{
    SWSS_LOG_ENTER();
    sai_object_type_t sai_object_type = HFTelUtils::group_name_to_sai_type(group_name);
    set<sai_stat_id_t> stats_ids_set;

    auto info = sai_metadata_get_object_type_info(sai_object_type);
    if (info == nullptr)
    {
        SWSS_LOG_THROW("Failed to get the object type info for %s", group_name.c_str());
    }

    auto state_enum = info->statenum;
    if (state_enum == nullptr)
    {
        SWSS_LOG_THROW("The object type %s does not support stats", group_name.c_str());
    }

    string type_prefix = "SAI_" + group_name + "_STAT_";

    for (size_t i = 0; i < state_enum->valuescount; i++)
    {
        if (object_counters.find(state_enum->valuesshortnames[i]) != object_counters.end())
        {
            SWSS_LOG_DEBUG("Found the object counter %s", state_enum->valuesshortnames[i]);
            stats_ids_set.insert(state_enum->values[i]);
        }
    }

    if (stats_ids_set.size() != object_counters.size())
    {
        SWSS_LOG_THROW("Failed to convert the object counters to stats ids for %s", group_name.c_str());
    }

    return stats_ids_set;
}

sai_stats_mode_t HFTelUtils::get_stats_mode(sai_object_type_t object_type, sai_stat_id_t stat_id)
{
    SWSS_LOG_ENTER();

    switch(object_type)
    {
        case SAI_OBJECT_TYPE_INGRESS_PRIORITY_GROUP:
            switch(stat_id)
            {
                case SAI_INGRESS_PRIORITY_GROUP_STAT_WATERMARK_BYTES:
                case SAI_INGRESS_PRIORITY_GROUP_STAT_SHARED_WATERMARK_BYTES:
                case SAI_INGRESS_PRIORITY_GROUP_STAT_XOFF_ROOM_WATERMARK_BYTES:
                    return SAI_STATS_MODE_READ_AND_CLEAR;
                default:
                    break;
            }
            break;
        case SAI_OBJECT_TYPE_BUFFER_POOL:
            switch(stat_id)
            {
                case SAI_BUFFER_POOL_STAT_WATERMARK_BYTES:
                case SAI_BUFFER_POOL_STAT_XOFF_ROOM_WATERMARK_BYTES:
                    return SAI_STATS_MODE_READ_AND_CLEAR;
                default:
                    break;
            }
            break;
        default:
            break;
    }

    return SAI_STATS_MODE_READ;
}
